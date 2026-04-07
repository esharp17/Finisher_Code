// ============================================================
//  Finisher Controller – based on proven sequential test sketch
//
//  Sequential ramp order:
//    UP:   Vibration → Planetary 1 → Planetary 2 → Central
//    DOWN: Central → Planetary 2 → Planetary 1 → Vibration
//  Each motor reaches target speed before the next one starts.
//
//  Motors (steppers):
//    Planetary 1 (NEMA 17) : PUL+ = D6,  DIR+ = D7
//    Planetary 2 (NEMA 17) : PUL+ = D2,  DIR+ = D3
//    Central     (NEMA 23) : PUL+ = D4,  DIR+ = D5
//
//  Vibration Motor (IBT_2 @ 12V):
//    RPWM = D8,  LPWM = D9
//
//  Serial commands (115200 baud, lowercase):
//    prpm <value>       – set planetary RPM (both P1 & P2)
//    crpm <value>       – set central RPM
//    vib <0-100>        – set vibration intensity as percentage
//    start              – sequential ramp-up to current targets
//    stop               – sequential ramp-down, cancel everything
//    pause              – sequential ramp-down, hold targets
//    resume             – sequential ramp-up to saved targets
//    cancel             – immediate stop + clear targets
//    sop                – Standard Operating Procedure
//    status             – print current state (also auto every 500ms)
// ============================================================

#include <AccelStepper.h>

// ---------- Pin Definitions ----------
#define PLANETARY1_PUL  6
#define PLANETARY1_DIR  7
#define PLANETARY2_PUL  2
#define PLANETARY2_DIR  3
#define CENTRAL_PUL     4
#define CENTRAL_DIR     5

#define VIB_RPWM  8
#define VIB_LPWM  9

// ---------- Motor Specs ----------
#define STEPS_PER_REV  200

// ---------- Ramp Time ----------
float RAMP_TIME_SECONDS = 5.0f;

// ---------- Settle threshold ----------
#define SETTLE_FRACTION  0.01f
#define SETTLE_ABS_MIN   2.0f

// ---------- AccelStepper Instances ----------
AccelStepper planetary1(AccelStepper::DRIVER, PLANETARY1_PUL, PLANETARY1_DIR);
AccelStepper planetary2(AccelStepper::DRIVER, PLANETARY2_PUL, PLANETARY2_DIR);
AccelStepper central   (AccelStepper::DRIVER, CENTRAL_PUL,    CENTRAL_DIR);

AccelStepper* motors[3]      = { &planetary1, &planetary2, &central };
const int     stepsPerRev[3] = { STEPS_PER_REV, STEPS_PER_REV, STEPS_PER_REV };

// ---------- Global State ----------
float  targetPlanetRPM   = 0.0f;
float  targetCentralRPM  = 0.0f;

uint8_t vibPercent  = 0;
bool    vibRunning  = false;

bool systemRunning = false;
bool systemPaused  = false;
bool sopActive     = false;

// Per-motor direction: +1 = CW, -1 = CCW
// P1 defaults to -1 (reversed wiring)
int dir1 = -1, dir2 = 1, dirC = 1;
int* dirs[3] = { &dir1, &dir2, &dirC };

// Saved targets for pause/resume
float savedPlanetRPM  = 0.0f;
float savedCentralRPM = 0.0f;
uint8_t savedVibPercent = 0;

float motorTargetSps[3] = { 0.0f, 0.0f, 0.0f };

// ---------- Sequencer ----------
// 4-step sequence: index 0=VIB, 1=P1, 2=P2, 3=Central
// Ramp-up goes 0→3, ramp-down goes 3→0
enum SeqState { SEQ_IDLE, SEQ_RAMPING };
SeqState seqState      = SEQ_IDLE;
int      seqStepIndex  = 0;
bool     seqGoingUp    = true;
#define  SEQ_STEP_COUNT 4
#define  SEQ_VIB_IDX    0

// Vibration settle time (ms)
#define VIB_SETTLE_MS   500
unsigned long vibSettleStart = 0;
bool vibSettling = false;

// ---------- Status Reporting ----------
unsigned long lastStatusMs = 0;
const unsigned long STATUS_PERIOD_MS = 500;

// ============================================================
//  Helpers
// ============================================================
float rpmToSps(float rpm, int spr) {
  return (rpm / 60.0f) * spr;
}

float absSpeed(AccelStepper& m) {
  float s = m.speed();
  return s < 0.0f ? -s : s;
}

// ============================================================
//  Vibration Motor
// ============================================================
void setVibration(bool enable) {
  if (enable && vibPercent > 0) {
    uint8_t pwm = (uint8_t)((vibPercent / 100.0f) * 255.0f);
    analogWrite(VIB_RPWM, pwm);
    digitalWrite(VIB_LPWM, LOW);
    vibRunning = true;
  } else {
    analogWrite(VIB_RPWM, 0);
    digitalWrite(VIB_LPWM, LOW);
    vibRunning = false;
  }
}

// ============================================================
//  Stepper Motor Ramp (identical to working test sketch)
// ============================================================
void applyRamp(int idx, float newTargetSps) {
  float currentSps = absSpeed(*motors[idx]);
  float delta      = newTargetSps - currentSps;
  if (delta < 0.0f) delta = -delta;

  float accel = (delta > 0.0f && RAMP_TIME_SECONDS > 0.0f)
                ? delta / RAMP_TIME_SECONDS
                : 1.0f;
  if (accel < 1.0f) accel = 1.0f;

  motors[idx]->setAcceleration(accel);
  motors[idx]->setMaxSpeed(newTargetSps);
  motors[idx]->moveTo((long)(*dirs[idx]) * 2000000000L);

  motorTargetSps[idx] = newTargetSps;
}

bool stepperSettled(int idx) {
  float target = motorTargetSps[idx];

  if (target < SETTLE_ABS_MIN) {
    return absSpeed(*motors[idx]) < SETTLE_ABS_MIN;
  }
  float threshold = target * SETTLE_FRACTION;
  if (threshold < SETTLE_ABS_MIN) threshold = SETTLE_ABS_MIN;
  return absSpeed(*motors[idx]) >= (target - threshold);
}

// ============================================================
//  Get target SPS for a stepper motor index (0=P1, 1=P2, 2=Central)
// ============================================================
float getTargetSps(int motorIdx) {
  if (motorIdx == 0 || motorIdx == 1) {
    return rpmToSps(targetPlanetRPM, stepsPerRev[motorIdx]);
  }
  return rpmToSps(targetCentralRPM, stepsPerRev[motorIdx]);
}

// ============================================================
//  Sequencer: 4-step (VIB, P1, P2, Central)
// ============================================================
void beginSequenceUp() {
  seqGoingUp   = true;
  seqStepIndex = 0;  // start at VIB
  seqState     = SEQ_RAMPING;

  // Turn on vibration immediately
  setVibration(true);
  vibSettleStart = millis();
  vibSettling    = true;

  Serial.println(F("[SEQ] Ramp UP: vibration on..."));
}

void beginSequenceDown() {
  seqGoingUp   = false;
  seqStepIndex = SEQ_STEP_COUNT - 1;  // start at Central (index 3)
  seqState     = SEQ_RAMPING;
  vibSettling  = false;

  // Start ramping central (motor index 2) to 0
  applyRamp(2, 0.0f);
  Serial.println(F("[SEQ] Ramp DOWN: central decelerating..."));
}

void sequencerTick() {
  if (seqState == SEQ_IDLE) return;

  if (seqGoingUp) {
    // ---- RAMP UP: VIB(0) → P1(1) → P2(2) → Central(3) ----
    if (seqStepIndex == SEQ_VIB_IDX) {
      // Vibration step: just wait for settle time
      if (vibSettling && (millis() - vibSettleStart >= VIB_SETTLE_MS)) {
        vibSettling = false;
        // Advance to P1
        seqStepIndex = 1;
        int motorIdx = 0;  // P1
        float sps = getTargetSps(motorIdx);
        applyRamp(motorIdx, sps);
        Serial.println(F("[SEQ] Ramp UP: planetary 1 accelerating..."));
      }
    } else {
      // Stepper steps: seqStepIndex 1=P1(motor0), 2=P2(motor1), 3=Central(motor2)
      int motorIdx = seqStepIndex - 1;
      if (stepperSettled(motorIdx)) {
        seqStepIndex++;
        if (seqStepIndex >= SEQ_STEP_COUNT) {
          seqState = SEQ_IDLE;
          Serial.println(F("[SEQ] All motors at target speed."));
          return;
        }
        int nextMotorIdx = seqStepIndex - 1;
        float sps = getTargetSps(nextMotorIdx);
        applyRamp(nextMotorIdx, sps);

        const char* names[] = { "planetary 1", "planetary 2", "central" };
        Serial.print(F("[SEQ] Ramp UP: "));
        Serial.print(names[nextMotorIdx]);
        Serial.println(F(" accelerating..."));
      }
    }
  } else {
    // ---- RAMP DOWN: Central(3) → P2(2) → P1(1) → VIB(0) ----
    if (seqStepIndex == SEQ_VIB_IDX) {
      // Vibration step: turn off and wait settle
      if (!vibSettling) {
        setVibration(false);
        vibSettleStart = millis();
        vibSettling    = true;
        Serial.println(F("[SEQ] Ramp DOWN: vibration off."));
      }
      if (vibSettling && (millis() - vibSettleStart >= VIB_SETTLE_MS)) {
        vibSettling = false;
        seqState = SEQ_IDLE;
        Serial.println(F("[SEQ] All motors stopped."));
      }
    } else {
      // Stepper steps: seqStepIndex 3=Central(motor2), 2=P2(motor1), 1=P1(motor0)
      int motorIdx = seqStepIndex - 1;
      if (stepperSettled(motorIdx)) {
        seqStepIndex--;
        if (seqStepIndex == SEQ_VIB_IDX) {
          // Next is vibration
          vibSettling = false;  // will be set in next tick
        } else {
          int nextMotorIdx = seqStepIndex - 1;
          applyRamp(nextMotorIdx, 0.0f);

          const char* names[] = { "planetary 1", "planetary 2", "central" };
          Serial.print(F("[SEQ] Ramp DOWN: "));
          Serial.print(names[nextMotorIdx]);
          Serial.println(F(" decelerating..."));
        }
      }
    }
  }
}

void immediateStop() {
  for (int i = 0; i < 3; i++) {
    motors[i]->setMaxSpeed(0);
    motors[i]->setAcceleration(10);
    motors[i]->moveTo(motors[i]->currentPosition());
    motorTargetSps[i] = 0.0f;
  }
  setVibration(false);
  seqState    = SEQ_IDLE;
  vibSettling = false;
}

// Apply new speeds to all stepper motors that are already running
// (used for mid-run RPM changes when sequencer is idle)
void refreshAllMotors() {
  for (int i = 0; i < 3; i++) {
    float sps = getTargetSps(i);
    applyRamp(i, sps);
  }
}

// ============================================================
//  Status Reporting (auto every 500ms)
// ============================================================
void emitStatusLine() {
  unsigned long now = millis();
  if (now - lastStatusMs < STATUS_PERIOD_MS) return;
  lastStatusMs = now;

  const char* st = "IDLE";
  if (systemPaused)       st = "PAUSED";
  else if (sopActive)     st = "SOP";
  else if (systemRunning) st = "RUNNING";

  float curPlanetRPM  = (absSpeed(planetary1) / STEPS_PER_REV) * 60.0f;
  float curCentralRPM = (absSpeed(central)    / STEPS_PER_REV) * 60.0f;

  Serial.print("STATUS state=");
  Serial.print(st);
  Serial.print(" planetRPM=");
  Serial.print(curPlanetRPM, 1);
  Serial.print(" centralRPM=");
  Serial.print(curCentralRPM, 1);
  Serial.print(" targetPlanetRPM=");
  Serial.print(targetPlanetRPM, 1);
  Serial.print(" targetCentralRPM=");
  Serial.print(targetCentralRPM, 1);
  Serial.print(" vib=");
  Serial.print(vibPercent);
  Serial.print(" paused=");
  Serial.print(systemPaused ? 1 : 0);
  Serial.print(" sop=");
  Serial.print(sopActive ? 1 : 0);
  Serial.print(" dir1=");
  Serial.print(dir1 == 1 ? "CW" : "CCW");
  Serial.print(" dir2=");
  Serial.print(dir2 == 1 ? "CW" : "CCW");
  Serial.print(" dirc=");
  Serial.print(dirC == 1 ? "CW" : "CCW");
  Serial.println();
}

// ============================================================
//  Serial Command Parser
// ============================================================
void parseCommand(String cmd) {
  cmd.trim();
  cmd.toLowerCase();

  // ---- prpm <value> ----
  if (cmd.startsWith("prpm ")) {
    float rpm = cmd.substring(5).toFloat();
    if (rpm < 0) rpm = 0;
    targetPlanetRPM = rpm;
    if (systemRunning && !systemPaused && seqState == SEQ_IDLE) {
      applyRamp(0, getTargetSps(0));
      applyRamp(1, getTargetSps(1));
    }
    Serial.print("[CMD] Planet RPM -> ");
    Serial.println(targetPlanetRPM);
    return;
  }

  // ---- crpm <value> ----
  if (cmd.startsWith("crpm ")) {
    float rpm = cmd.substring(5).toFloat();
    if (rpm < 0) rpm = 0;
    targetCentralRPM = rpm;
    if (systemRunning && !systemPaused && seqState == SEQ_IDLE) {
      applyRamp(2, getTargetSps(2));
    }
    Serial.print("[CMD] Central RPM -> ");
    Serial.println(targetCentralRPM);
    return;
  }

  // ---- vib <0-100> ----
  if (cmd.startsWith("vib ")) {
    int val = cmd.substring(4).toInt();
    if (val < 0)   val = 0;
    if (val > 100) val = 100;
    vibPercent = (uint8_t)val;
    if (vibRunning) setVibration(true);
    Serial.print("[CMD] Vib -> ");
    Serial.print(vibPercent);
    Serial.println("%");
    return;
  }

  // ---- start ----
  if (cmd == "start") {
    systemRunning = true;
    systemPaused  = false;
    beginSequenceUp();
    Serial.println("[CMD] START (sequential ramp-up)");
    return;
  }

  // ---- stop ----
  if (cmd == "stop") {
    systemRunning = false;
    systemPaused  = false;
    sopActive     = false;
    targetPlanetRPM  = 0.0f;
    targetCentralRPM = 0.0f;
    beginSequenceDown();
    Serial.println("[CMD] STOP (sequential ramp-down)");
    return;
  }

  // ---- pause ----
  if (cmd == "pause") {
    if (systemRunning && !systemPaused) {
      systemPaused    = true;
      savedPlanetRPM  = targetPlanetRPM;
      savedCentralRPM = targetCentralRPM;
      savedVibPercent = vibPercent;
      beginSequenceDown();
      Serial.println("[CMD] PAUSED (sequential ramp-down)");
    }
    return;
  }

  // ---- resume ----
  if (cmd == "resume") {
    if (systemPaused) {
      systemPaused     = false;
      targetPlanetRPM  = savedPlanetRPM;
      targetCentralRPM = savedCentralRPM;
      vibPercent       = savedVibPercent;
      beginSequenceUp();
      Serial.println("[CMD] RESUMED (sequential ramp-up)");
    }
    return;
  }

  // ---- cancel ----
  if (cmd == "cancel") {
    systemRunning    = false;
    systemPaused     = false;
    sopActive        = false;
    targetPlanetRPM  = 0.0f;
    targetCentralRPM = 0.0f;
    savedPlanetRPM   = 0.0f;
    savedCentralRPM  = 0.0f;
    immediateStop();
    Serial.println("[CMD] CANCELLED (immediate stop)");
    return;
  }

  // ---- sop ----
  if (cmd == "sop") {
    sopActive        = true;
    systemRunning    = true;
    systemPaused     = false;
    targetPlanetRPM  = 300.0f;
    targetCentralRPM = 280.0f;
    beginSequenceUp();
    Serial.println("[CMD] SOP started (sequential ramp-up)");
    return;
  }

  // ---- dir1 cw/ccw ----
  if (cmd == "dir1 cw")  { dir1 =  1; Serial.println(F("[CMD] P1 -> CW"));  return; }
  if (cmd == "dir1 ccw") { dir1 = -1; Serial.println(F("[CMD] P1 -> CCW")); return; }

  // ---- dir2 cw/ccw ----
  if (cmd == "dir2 cw")  { dir2 =  1; Serial.println(F("[CMD] P2 -> CW"));  return; }
  if (cmd == "dir2 ccw") { dir2 = -1; Serial.println(F("[CMD] P2 -> CCW")); return; }

  // ---- dirc cw/ccw ----
  if (cmd == "dirc cw")  { dirC =  1; Serial.println(F("[CMD] Central -> CW"));  return; }
  if (cmd == "dirc ccw") { dirC = -1; Serial.println(F("[CMD] Central -> CCW")); return; }

  // ---- dirall cw/ccw ----
  if (cmd == "dirall cw")  { dir1 = dir2 = dirC =  1; Serial.println(F("[CMD] All -> CW"));  return; }
  if (cmd == "dirall ccw") { dir1 = dir2 = dirC = -1; Serial.println(F("[CMD] All -> CCW")); return; }

  // ---- reverse (flip all stepper directions) ----
  if (cmd == "reverse") {
    dir1 = -dir1;
    dir2 = -dir2;
    dirC = -dirC;
    // If motors are running, re-apply with new direction
    if (systemRunning && !systemPaused && seqState == SEQ_IDLE) {
      for (int i = 0; i < 3; i++) {
        applyRamp(i, motorTargetSps[i]);
      }
    }
    Serial.println(F("[CMD] All directions reversed"));
    return;
  }

  // ---- status ----
  if (cmd == "status") {
    lastStatusMs = 0;
    emitStatusLine();
    return;
  }

  Serial.print("[ERR] Unknown: ");
  Serial.println(cmd);
}

// ============================================================
void setup() {
  Serial.begin(115200);
  while (!Serial) {}

  pinMode(VIB_RPWM, OUTPUT);
  pinMode(VIB_LPWM, OUTPUT);
  analogWrite(VIB_RPWM, 0);
  digitalWrite(VIB_LPWM, LOW);

  for (int i = 0; i < 3; i++) {
    motors[i]->setMaxSpeed(0);
    motors[i]->setAcceleration(10);
    motors[i]->moveTo(2000000000L);
    motorTargetSps[i] = 0.0f;
  }

  Serial.println(F("========================================"));
  Serial.println(F("  Finisher Controller Ready"));
  Serial.println(F("  Sequential: VIB -> P1 -> P2 -> Central"));
  Serial.println(F("  Commands: prpm/crpm/vib/start/stop"));
  Serial.println(F("            pause/resume/cancel/sop"));
  Serial.println(F("========================================"));
}

// ============================================================
void loop() {
  planetary1.run();
  planetary2.run();
  central.run();

  sequencerTick();
  emitStatusLine();

  static String buf = "";
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (buf.length() > 0) { parseCommand(buf); buf = ""; }
    } else {
      buf += c;
    }
  }
}