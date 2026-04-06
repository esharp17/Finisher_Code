// ============================================================
//  Finisher Controller – AccelStepper w/ DM556 Drivers
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
//                         (planet 300, central 280, 3h timer)
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

// ---------- Ramp ----------
float RAMP_TIME_SECONDS = 5.0f;

// ---------- AccelStepper Instances ----------
AccelStepper planetary1(AccelStepper::DRIVER, PLANETARY1_PUL, PLANETARY1_DIR);
AccelStepper planetary2(AccelStepper::DRIVER, PLANETARY2_PUL, PLANETARY2_DIR);
AccelStepper central   (AccelStepper::DRIVER, CENTRAL_PUL,    CENTRAL_DIR);

// ---------- Global State ----------
float  targetPlanetRPM  = 0.0f;
float  targetCentralRPM = 0.0f;
float  currentPlanetRPM = 0.0f;
float  currentCentralRPM = 0.0f;

uint8_t vibPercent   = 0;
bool    vibRunning   = false;

bool systemRunning = false;
bool systemPaused  = false;
bool sopActive     = false;

// Saved targets for pause/resume
float savedPlanetRPM  = 0.0f;
float savedCentralRPM = 0.0f;

// ---------- Ramp State ----------
bool  rampPlanetActive  = false;
bool  rampCentralActive = false;
float p_startRPM = 0, p_goalRPM = 0;
float c_startRPM = 0, c_goalRPM = 0;
unsigned long p_rampStartMs = 0;
unsigned long c_rampStartMs = 0;

// ---------- Status Reporting ----------
unsigned long lastStatusMs = 0;
const unsigned long STATUS_PERIOD_MS = 500;

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
//  Stepper Helpers
// ============================================================
float rpmToSps(float rpm) {
  return (rpm / 60.0f) * STEPS_PER_REV;
}

void applyPlanetSpeed(float rpm) {
  float spd = rpmToSps(rpm);
  float acc = (spd > 0 && RAMP_TIME_SECONDS > 0) ? spd / RAMP_TIME_SECONDS : 100.0f;
  if (acc < 10.0f) acc = 10.0f;

  planetary1.setMaxSpeed(spd > 0 ? spd : 1);
  planetary1.setAcceleration(acc);
  planetary1.moveTo((long)2000000000L);

  planetary2.setMaxSpeed(spd > 0 ? spd : 1);
  planetary2.setAcceleration(acc);
  planetary2.moveTo((long)2000000000L);
}

void applyCentralSpeed(float rpm) {
  float spd = rpmToSps(rpm);
  float acc = (spd > 0 && RAMP_TIME_SECONDS > 0) ? spd / RAMP_TIME_SECONDS : 100.0f;
  if (acc < 10.0f) acc = 10.0f;

  central.setMaxSpeed(spd > 0 ? spd : 1);
  central.setAcceleration(acc);
  central.moveTo((long)2000000000L);
}

void startRampPlanet(float fromRPM, float toRPM) {
  p_startRPM = fromRPM;
  p_goalRPM  = toRPM;
  p_rampStartMs = millis();
  rampPlanetActive = true;
}

void startRampCentral(float fromRPM, float toRPM) {
  c_startRPM = fromRPM;
  c_goalRPM  = toRPM;
  c_rampStartMs = millis();
  rampCentralActive = true;
}

void updateRamps() {
  unsigned long now = millis();

  if (rampPlanetActive) {
    float t = (now - p_rampStartMs) / 1000.0f / RAMP_TIME_SECONDS;
    if (t >= 1.0f) {
      rampPlanetActive = false;
      currentPlanetRPM = p_goalRPM;
    } else {
      currentPlanetRPM = p_startRPM + (p_goalRPM - p_startRPM) * t;
    }
    applyPlanetSpeed(currentPlanetRPM);
  }

  if (rampCentralActive) {
    float t = (now - c_rampStartMs) / 1000.0f / RAMP_TIME_SECONDS;
    if (t >= 1.0f) {
      rampCentralActive = false;
      currentCentralRPM = c_goalRPM;
    } else {
      currentCentralRPM = c_startRPM + (c_goalRPM - c_startRPM) * t;
    }
    applyCentralSpeed(currentCentralRPM);
  }
}

void rampToTargets() {
  startRampPlanet(currentPlanetRPM, targetPlanetRPM);
  startRampCentral(currentCentralRPM, targetCentralRPM);
}

void rampToZero() {
  startRampPlanet(currentPlanetRPM, 0.0f);
  startRampCentral(currentCentralRPM, 0.0f);
}

void immediateStop() {
  rampPlanetActive  = false;
  rampCentralActive = false;
  currentPlanetRPM  = 0.0f;
  currentCentralRPM = 0.0f;

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
  if (systemPaused) st = "PAUSED";
  else if (sopActive) st = "SOP";
  else if (systemRunning) st = "RUNNING";

  Serial.print("STATUS state=");
  Serial.print(st);
  Serial.print(" planetRPM=");
  Serial.print(currentPlanetRPM, 1);
  Serial.print(" centralRPM=");
  Serial.print(currentCentralRPM, 1);
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
      startRampPlanet(currentPlanetRPM, targetPlanetRPM);
    }
    Serial.print("[CMD] Planet RPM → ");
    Serial.println(targetPlanetRPM);
    return;
  }

  // ---- crpm <value> ----
  if (cmd.startsWith("crpm ")) {
    float rpm = cmd.substring(5).toFloat();
    if (rpm < 0) rpm = 0;
    targetCentralRPM = rpm;
    if (systemRunning && !systemPaused) {
      startRampCentral(currentCentralRPM, targetCentralRPM);
    }
    Serial.print("[CMD] Central RPM → ");
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
    Serial.print("[CMD] Vib → ");
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

  planetary1.setMaxSpeed(1);
  planetary1.setAcceleration(10);
  planetary2.setMaxSpeed(1);
  planetary2.setAcceleration(10);
  central.setMaxSpeed(1);
  central.setAcceleration(10);

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

  updateRamps();
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