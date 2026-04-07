// ============================================================
//  Finisher Controller – AccelStepper Position-Mode w/ DM556
//
//  Sequential motor ramp-up order:
//    1. Vibration motor  (instant PWM, brief settle)
//    2. Planetary 1      (ramp to full speed)
//    3. Planetary 2      (ramp to full speed)
//    4. Central          (ramp to full speed)
//  Each motor reaches target speed before the next one starts.
//  Ramp-down is reverse order: Central → P2 → P1 → Vibration.
//
//  Motors:
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
float RAMP_TIME_SECONDS = 8.0f;

// ---------- Settle Threshold ----------
// Motor is "at speed" when within this fraction of target
#define SETTLE_FRACTION  0.02f
#define SETTLE_ABS_MIN   3.0f

// ---------- AccelStepper Instances ----------
AccelStepper planetary1(AccelStepper::DRIVER, PLANETARY1_PUL, PLANETARY1_DIR);
AccelStepper planetary2(AccelStepper::DRIVER, PLANETARY2_PUL, PLANETARY2_DIR);
AccelStepper central   (AccelStepper::DRIVER, CENTRAL_PUL,    CENTRAL_DIR);

// ---------- Global State ----------
float  targetPlanetRPM   = 0.0f;
float  targetCentralRPM  = 0.0f;

uint8_t vibPercent = 0;
bool    vibRunning = false;

bool systemRunning = false;
bool systemPaused  = false;
bool sopActive     = false;

// Saved targets for pause/resume
float savedPlanetRPM  = 0.0f;
float savedCentralRPM = 0.0f;

// ---------- Sequential Ramp Sequencer ----------
// Ramp-up:   VIB → P1 → P2 → CENTRAL → DONE
// Ramp-down: CENTRAL → P2 → P1 → VIB → DONE
enum SeqStage {
  SEQ_IDLE,
  SEQ_VIB,
  SEQ_P1,
  SEQ_P2,
  SEQ_CENTRAL,
  SEQ_DONE
};

enum SeqDir { SEQ_UP, SEQ_DOWN };

SeqStage  seqStage = SEQ_IDLE;
SeqDir    seqDir   = SEQ_UP;
unsigned long seqStageStartMs = 0;

// Vibration settle time (ms) — vib is instant PWM, just a brief pause
#define VIB_SETTLE_MS 500

// ---------- Status Reporting ----------
unsigned long lastStatusMs = 0;
const unsigned long STATUS_PERIOD_MS = 500;

// ============================================================
//  Helpers
// ============================================================
float rpmToSps(float rpm) {
  return (rpm / 60.0f) * STEPS_PER_REV;
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
//  Motor Speed Application
//  Uses the proven test sketch pattern:
//    setAcceleration → setMaxSpeed → moveTo(far target)
// ============================================================
void applyMotorSpeed(AccelStepper& motor, float newSps) {
  float currentSps = absSpeed(motor);
  float delta = newSps - currentSps;
  if (delta < 0.0f) delta = -delta;

  float accel = (delta > 0.0f && RAMP_TIME_SECONDS > 0.0f)
                ? delta / RAMP_TIME_SECONDS
                : 10.0f;
  if (accel < 10.0f) accel = 10.0f;

  motor.setAcceleration(accel);

  if (newSps < 1.0f) {
    motor.setMaxSpeed(max(absSpeed(motor), 1.0f));
    motor.moveTo(motor.currentPosition());
  } else {
    motor.setMaxSpeed(newSps);
    motor.moveTo((long)2000000000L);
  }
}

// ============================================================
//  Motor Settled Check
//  Returns true when motor is within threshold of target speed.
//  For stopping (targetSps == 0), returns true when speed < min.
// ============================================================
bool motorSettled(AccelStepper& motor, float targetSps) {
  float cur = absSpeed(motor);
  if (targetSps < 1.0f) {
    return cur < SETTLE_ABS_MIN;
  }
  float threshold = targetSps * SETTLE_FRACTION;
  if (threshold < SETTLE_ABS_MIN) threshold = SETTLE_ABS_MIN;
  return cur >= (targetSps - threshold);
}

void immediateStop() {
  planetary1.setMaxSpeed(1);
  planetary1.moveTo(planetary1.currentPosition());
  planetary2.setMaxSpeed(1);
  planetary2.moveTo(planetary2.currentPosition());
  central.setMaxSpeed(1);
  central.moveTo(central.currentPosition());
  setVibration(false);
  seqStage = SEQ_IDLE;
}

// ============================================================
//  Sequential Ramp Sequencer
// ============================================================
void beginSequence(SeqDir dir) {
  seqDir = dir;
  seqStageStartMs = millis();

  if (dir == SEQ_UP) {
    // Start with vibration
    seqStage = SEQ_VIB;
    setVibration(true);
    Serial.println(F("[SEQ] Ramp UP: vibration on..."));
  } else {
    // Ramp down: start with central
    seqStage = SEQ_CENTRAL;
    applyMotorSpeed(central, 0.0f);
    Serial.println(F("[SEQ] Ramp DOWN: central decelerating..."));
  }
}

void sequencerTick() {
  if (seqStage == SEQ_IDLE || seqStage == SEQ_DONE) return;

  unsigned long elapsed = millis() - seqStageStartMs;

  if (seqDir == SEQ_UP) {
    // ---- RAMP UP: VIB → P1 → P2 → CENTRAL ----
    switch (seqStage) {
      case SEQ_VIB:
        if (elapsed >= VIB_SETTLE_MS) {
          seqStage = SEQ_P1;
          seqStageStartMs = millis();
          applyMotorSpeed(planetary1, rpmToSps(targetPlanetRPM));
          Serial.println(F("[SEQ] Ramp UP: planetary 1 accelerating..."));
        }
        break;

      case SEQ_P1:
        if (motorSettled(planetary1, rpmToSps(targetPlanetRPM))) {
          seqStage = SEQ_P2;
          seqStageStartMs = millis();
          applyMotorSpeed(planetary2, rpmToSps(targetPlanetRPM));
          Serial.println(F("[SEQ] Ramp UP: planetary 2 accelerating..."));
        }
        break;

      case SEQ_P2:
        if (motorSettled(planetary2, rpmToSps(targetPlanetRPM))) {
          seqStage = SEQ_CENTRAL;
          seqStageStartMs = millis();
          applyMotorSpeed(central, rpmToSps(targetCentralRPM));
          Serial.println(F("[SEQ] Ramp UP: central accelerating..."));
        }
        break;

      case SEQ_CENTRAL:
        if (motorSettled(central, rpmToSps(targetCentralRPM))) {
          seqStage = SEQ_DONE;
          Serial.println(F("[SEQ] All motors at target speed."));
        }
        break;

      default: break;
    }
  } else {
    // ---- RAMP DOWN: CENTRAL → P2 → P1 → VIB ----
    switch (seqStage) {
      case SEQ_CENTRAL:
        if (motorSettled(central, 0.0f)) {
          seqStage = SEQ_P2;
          seqStageStartMs = millis();
          applyMotorSpeed(planetary2, 0.0f);
          Serial.println(F("[SEQ] Ramp DOWN: planetary 2 decelerating..."));
        }
        break;

      case SEQ_P2:
        if (motorSettled(planetary2, 0.0f)) {
          seqStage = SEQ_P1;
          seqStageStartMs = millis();
          applyMotorSpeed(planetary1, 0.0f);
          Serial.println(F("[SEQ] Ramp DOWN: planetary 1 decelerating..."));
        }
        break;

      case SEQ_P1:
        if (motorSettled(planetary1, 0.0f)) {
          seqStage = SEQ_VIB;
          seqStageStartMs = millis();
          setVibration(false);
          Serial.println(F("[SEQ] Ramp DOWN: vibration off."));
        }
        break;

      case SEQ_VIB:
        if (elapsed >= VIB_SETTLE_MS) {
          seqStage = SEQ_DONE;
          Serial.println(F("[SEQ] All motors stopped."));
        }
        break;

      default: break;
    }
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

  float curPlanet  = absSpeed(planetary1);
  float curCentral = absSpeed(central);
  float curPlanetRPM  = (curPlanet  / STEPS_PER_REV) * 60.0f;
  float curCentralRPM = (curCentral / STEPS_PER_REV) * 60.0f;

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
    // If running and sequencer is done, apply immediately
    if (systemRunning && !systemPaused && seqStage == SEQ_DONE) {
      applyMotorSpeed(planetary1, rpmToSps(targetPlanetRPM));
      applyMotorSpeed(planetary2, rpmToSps(targetPlanetRPM));
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
    // If running and sequencer is done, apply immediately
    if (systemRunning && !systemPaused && seqStage == SEQ_DONE) {
      applyMotorSpeed(central, rpmToSps(targetCentralRPM));
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
    beginSequence(SEQ_UP);
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
    beginSequence(SEQ_DOWN);
    Serial.println("[CMD] STOP (sequential ramp-down)");
    return;
  }

  // ---- pause ----
  if (cmd == "pause") {
    if (systemRunning && !systemPaused) {
      systemPaused = true;
      savedPlanetRPM  = targetPlanetRPM;
      savedCentralRPM = targetCentralRPM;
      beginSequence(SEQ_DOWN);
      Serial.println("[CMD] PAUSED (sequential ramp-down)");
    }
    return;
  }

  // ---- resume ----
  if (cmd == "resume") {
    if (systemPaused) {
      systemPaused = false;
      targetPlanetRPM  = savedPlanetRPM;
      targetCentralRPM = savedCentralRPM;
      beginSequence(SEQ_UP);
      Serial.println("[CMD] RESUMED (sequential ramp-up)");
    }
    return;
  }

  // ---- cancel ----
  if (cmd == "cancel") {
    systemRunning = false;
    systemPaused  = false;
    sopActive     = false;
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
    sopActive = true;
    systemRunning = true;
    systemPaused  = false;
    targetPlanetRPM  = 300.0f;
    targetCentralRPM = 280.0f;
    beginSequence(SEQ_UP);
    Serial.println("[CMD] SOP started (sequential ramp-up)");
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
    AccelStepper* m = (i == 0) ? &planetary1 : (i == 1) ? &planetary2 : &central;
    m->setMaxSpeed(1);
    m->setAcceleration(10);
    m->moveTo(2000000000L);
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