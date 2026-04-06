// ============================================================
//  Finisher Controller – AccelStepper Position-Mode w/ DM556
//
//  Uses the same proven motor control pattern as the working
//  test sketch: run() + setMaxSpeed() + setAcceleration() +
//  moveTo().  Acceleration is computed from the ramp time so
//  that speed changes are always smooth.
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
//    start              – ramp motors to current target RPMs
//    stop               – ramp all to 0, cancel everything
//    pause              – ramp to 0, hold targets for resume
//    resume             – ramp back to saved targets
//    cancel             – full stop + clear targets
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
// How many seconds for a 0→max speed ramp.  Smaller changes
// use proportionally shorter times (minimum 2 s).
float RAMP_TIME_SECONDS = 8.0f;

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
//  Uses the same pattern as the proven test sketch:
//    setAcceleration → setMaxSpeed → moveTo(far target)
//  AccelStepper handles the smooth acceleration internally.
// ============================================================
void applyMotorSpeed(AccelStepper& motor, float newSps) {
  float currentSps = absSpeed(motor);
  float delta = newSps - currentSps;
  if (delta < 0.0f) delta = -delta;

  // Calculate acceleration from ramp time
  float accel = (delta > 0.0f && RAMP_TIME_SECONDS > 0.0f)
                ? delta / RAMP_TIME_SECONDS
                : 10.0f;
  if (accel < 10.0f) accel = 10.0f;

  motor.setAcceleration(accel);

  if (newSps < 1.0f) {
    // Ramp down to stop
    motor.setMaxSpeed(max(absSpeed(motor), 1.0f));
    motor.moveTo(motor.currentPosition());
  } else {
    motor.setMaxSpeed(newSps);
    motor.moveTo((long)2000000000L);
  }
}

void applyPlanetSpeed(float rpm) {
  float sps = rpmToSps(rpm);
  applyMotorSpeed(planetary1, sps);
  applyMotorSpeed(planetary2, sps);
}

void applyCentralSpeed(float rpm) {
  float sps = rpmToSps(rpm);
  applyMotorSpeed(central, sps);
}

void rampToTargets() {
  applyPlanetSpeed(targetPlanetRPM);
  applyCentralSpeed(targetCentralRPM);
}

void rampToZero() {
  applyPlanetSpeed(0.0f);
  applyCentralSpeed(0.0f);
}

void immediateStop() {
  planetary1.setMaxSpeed(1);
  planetary1.moveTo(planetary1.currentPosition());
  planetary2.setMaxSpeed(1);
  planetary2.moveTo(planetary2.currentPosition());
  central.setMaxSpeed(1);
  central.moveTo(central.currentPosition());

  setVibration(false);
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
    if (systemRunning && !systemPaused) {
      applyPlanetSpeed(targetPlanetRPM);
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
    if (systemRunning && !systemPaused) {
      applyCentralSpeed(targetCentralRPM);
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
    rampToTargets();
    setVibration(true);
    Serial.println("[CMD] START");
    return;
  }

  // ---- stop ----
  if (cmd == "stop") {
    systemRunning = false;
    systemPaused  = false;
    sopActive     = false;
    targetPlanetRPM  = 0.0f;
    targetCentralRPM = 0.0f;
    rampToZero();
    setVibration(false);
    Serial.println("[CMD] STOP");
    return;
  }

  // ---- pause ----
  if (cmd == "pause") {
    if (systemRunning && !systemPaused) {
      systemPaused = true;
      savedPlanetRPM  = targetPlanetRPM;
      savedCentralRPM = targetCentralRPM;
      rampToZero();
      setVibration(false);
      Serial.println("[CMD] PAUSED");
    }
    return;
  }

  // ---- resume ----
  if (cmd == "resume") {
    if (systemPaused) {
      systemPaused = false;
      targetPlanetRPM  = savedPlanetRPM;
      targetCentralRPM = savedCentralRPM;
      rampToTargets();
      setVibration(true);
      Serial.println("[CMD] RESUMED");
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
    Serial.println("[CMD] CANCELLED");
    return;
  }

  // ---- sop ----
  if (cmd == "sop") {
    sopActive = true;
    systemRunning = true;
    systemPaused  = false;
    targetPlanetRPM  = 300.0f;
    targetCentralRPM = 280.0f;
    rampToTargets();
    setVibration(true);
    Serial.println("[CMD] SOP started (Planet 300, Central 280)");
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
  Serial.println(F("  Commands: prpm/crpm/vib/start/stop"));
  Serial.println(F("            pause/resume/cancel/sop"));
  Serial.println(F("========================================"));
}

// ============================================================
void loop() {
  planetary1.run();
  planetary2.run();
  central.run();

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