#include <AccelStepper.h>

// ── Pin Definitions ────────────────────────────────────────────────────────────
#define PLANETARY1_PUL  6
#define PLANETARY1_DIR  5

#define CENTRAL_PUL     2
#define CENTRAL_DIR     3

#define VIB_RPWM        9
#define VIB_LPWM        10

#define ESTOP_PIN       15

// ── Constants ─────────────────────────────────────────────────────────────────
#define STEPS_PER_REV     200
#define RAMP_DURATION_MS  5000UL   // 5 seconds

// ── Motor Objects ─────────────────────────────────────────────────────────────
AccelStepper Central_Motor(AccelStepper::DRIVER, CENTRAL_PUL, CENTRAL_DIR);
AccelStepper Planetary_Motor(AccelStepper::DRIVER, PLANETARY1_PUL, PLANETARY1_DIR);

// ── Ramp State Struct ─────────────────────────────────────────────────────────
struct RampState {
  float currentVal;    // what is actually being output right now
  float startVal;      // value when the ramp began
  float targetVal;     // value we are ramping toward
  unsigned long startTime;
  bool ramping;
};

RampState centralRamp    = {0, 0, 0, 0, false};
RampState planetaryRamp  = {0, 0, 0, 0, false};
RampState vibRamp        = {0, 0, 0, 0, false};

// ── Sequenced Startup ─────────────────────────────────────────────────────────
// On first START command, motors come up one after another:
//   0–5s   : vibration ramps to target
//   5–10s  : planetary ramps to target
//   10–15s : central ramps to target
enum StartupPhase { IDLE, VIB_RAMP, PLANETARY_RAMP, CENTRAL_RAMP, RUNNING };
StartupPhase startupPhase = IDLE;
unsigned long phaseStartTime = 0;

// Saved targets for sequenced startup
float pendingCentralRPM   = 0;
float pendingPlanetaryRPM = 0;
float pendingVibPWM       = 0;
bool  vibForward          = true;

// ── Emergency Stop ───────────────────────────────────────────────────────────
// Pin 15 wired to N.C. E-stop switch to GND. INPUT_PULLUP: LOW = triggered.
bool estopActive = false;
unsigned long estopLastChangeMs = 0;
int  estopLastRead = HIGH;
const unsigned long ESTOP_DEBOUNCE_MS = 20;

// ── Status Reporting ─────────────────────────────────────────────────────────
unsigned long lastStatusMs = 0;
const unsigned long STATUS_PERIOD_MS = 500;

void emitStatusLine() {
  unsigned long now = millis();
  if (now - lastStatusMs < STATUS_PERIOD_MS) return;
  lastStatusMs = now;

  const char* phaseNames[] = {"IDLE","VIB_RAMP","PLANETARY_RAMP","CENTRAL_RAMP","RUNNING"};
  float curPlanetRPM  = (planetaryRamp.currentVal / STEPS_PER_REV) * 60.0f;
  float curCentralRPM = (centralRamp.currentVal / STEPS_PER_REV) * 60.0f;

  Serial.print("STATUS state=");
  Serial.print(phaseNames[startupPhase]);
  Serial.print(" planetRPM=");
  Serial.print(curPlanetRPM, 1);
  Serial.print(" centralRPM=");
  Serial.print(curCentralRPM, 1);
  Serial.print(" targetPlanetRPM=");
  Serial.print(pendingPlanetaryRPM, 1);
  Serial.print(" targetCentralRPM=");
  Serial.print(pendingCentralRPM, 1);
  Serial.print(" vib=");
  Serial.print((int)vibRamp.currentVal);
  Serial.print(" vibTarget=");
  Serial.print((int)pendingVibPWM);
  Serial.print(" vibDir=");
  Serial.print(vibForward ? "F" : "R");
  Serial.print(" estop=");
  Serial.print(estopActive ? "A" : "C");
  Serial.println();
}

// ── E-stop check (debounced) ─────────────────────────────────────────────────
void checkEstop() {
  int r = digitalRead(ESTOP_PIN);
  unsigned long now = millis();
  if (r != estopLastRead) {
    estopLastRead = r;
    estopLastChangeMs = now;
    return;
  }
  if (now - estopLastChangeMs < ESTOP_DEBOUNCE_MS) return;

  bool trigger = (r == LOW);
  if (trigger && !estopActive) {
    estopActive = true;
    // Kill everything instantly (motors already powerless via hardware, but clear state)
    vibRamp.currentVal = 0;       vibRamp.ramping = false;       vibRamp.targetVal = 0;
    centralRamp.currentVal = 0;   centralRamp.ramping = false;   centralRamp.targetVal = 0;
    planetaryRamp.currentVal = 0; planetaryRamp.ramping = false; planetaryRamp.targetVal = 0;
    pendingCentralRPM = 0;
    pendingPlanetaryRPM = 0;
    pendingVibPWM = 0;
    startupPhase = IDLE;
    setVibMotor(0, vibForward);
    Serial.println("ESTOP:ACTIVE");
  } else if (!trigger && estopActive) {
    estopActive = false;
    Serial.println("ESTOP:CLEAR");
  }
}

// ── Helpers ───────────────────────────────────────────────────────────────────
float rpmToStepsPerSec(float rpm) {
  return (rpm * STEPS_PER_REV) / 60.0;
}

void setVibMotor(int pwmVal, bool forward) {
  pwmVal = constrain(pwmVal, 0, 255);
  if (forward) {
    analogWrite(VIB_RPWM, pwmVal);
    analogWrite(VIB_LPWM, 0);
  } else {
    analogWrite(VIB_RPWM, 0);
    analogWrite(VIB_LPWM, pwmVal);
  }
}

// Start a ramp from wherever the motor currently is toward a new target
void startRamp(RampState &r, float target) {
  r.startVal   = r.currentVal;
  r.targetVal  = target;
  r.startTime  = millis();
  r.ramping    = (r.currentVal != target);
}

// Call every loop — updates currentVal along the ramp curve, returns true while still moving
bool updateRamp(RampState &r) {
  if (!r.ramping) return false;

  unsigned long elapsed = millis() - r.startTime;
  if (elapsed >= RAMP_DURATION_MS) {
    r.currentVal = r.targetVal;
    r.ramping    = false;
    return false;
  }

  // Linear interpolation
  float t      = (float)elapsed / (float)RAMP_DURATION_MS;
  r.currentVal = r.startVal + t * (r.targetVal - r.startVal);
  return true;
}

// ── Sequenced Startup Logic ───────────────────────────────────────────────────
void beginStartupSequence() {
  startupPhase  = VIB_RAMP;
  phaseStartTime = millis();
  startRamp(vibRamp, pendingVibPWM);
  Serial.println("[SEQ] Phase 1/3 — Vibration ramping up...");
}

void updateStartupSequence() {
  if (startupPhase == IDLE || startupPhase == RUNNING) return;

  unsigned long elapsed = millis() - phaseStartTime;

  switch (startupPhase) {
    case VIB_RAMP:
      if (elapsed >= RAMP_DURATION_MS) {
        vibRamp.currentVal = pendingVibPWM;
        vibRamp.ramping    = false;
        startupPhase       = PLANETARY_RAMP;
        phaseStartTime     = millis();
        startRamp(planetaryRamp, rpmToStepsPerSec(pendingPlanetaryRPM));
        Serial.println("[SEQ] Phase 2/3 — Planetary Motor ramping up...");
      }
      break;

    case PLANETARY_RAMP:
      if (elapsed >= RAMP_DURATION_MS) {
        planetaryRamp.currentVal = rpmToStepsPerSec(pendingPlanetaryRPM);
        planetaryRamp.ramping    = false;
        startupPhase             = CENTRAL_RAMP;
        phaseStartTime           = millis();
        startRamp(centralRamp, rpmToStepsPerSec(pendingCentralRPM));
        Serial.println("[SEQ] Phase 3/3 — Central Motor ramping up...");
      }
      break;

    case CENTRAL_RAMP:
      if (elapsed >= RAMP_DURATION_MS) {
        centralRamp.currentVal = rpmToStepsPerSec(pendingCentralRPM);
        centralRamp.ramping    = false;
        startupPhase           = RUNNING;
        Serial.println("[SEQ] Startup complete — all motors at target speed.");
      }
      break;

    default:
      break;
  }
}

// ── Serial Command Parser ──────────────────────────────────────────────────────
// Commands:
//   START            Begin sequenced startup ramp (uses last set C/P/V targets)
//   C:<rpm>          Set Central Motor RPM    (live ramp if already running)
//   P:<rpm>          Set Planetary Motor RPM  (live ramp if already running)
//   V:<0-255>        Set Vibration PWM speed  (live ramp if already running)
//   VD:<F|R>         Set Vibration direction
//   STOP             Ramp all motors down over 5 s
//   STATUS           Print current state
//
void parseCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  // ── SOP1 ───────────────────────────────────────────────────────────────────
  if (cmd == "SOP1") {
    if (estopActive) {
      Serial.println("[ERR] E-STOP active — disarm before starting");
      return;
    }
    pendingPlanetaryRPM = -300;
    pendingCentralRPM   = 280;
    pendingVibPWM       = 211;
    beginStartupSequence();
    Serial.println("[SOP1] Standard cycle: P:300 RPM | C:280 RPM | V:211 PWM (83%)");
    Serial.println("[SOP1] Sequenced ramp starting...");
    return;
  }

  // ── START ──────────────────────────────────────────────────────────────────
  if (cmd == "START") {
    if (estopActive) {
      Serial.println("[ERR] E-STOP active — disarm before starting");
      return;
    }
    if (startupPhase != IDLE && startupPhase != RUNNING) {
      Serial.println("[WARN] Startup already in progress.");
      return;
    }
    beginStartupSequence();
    return;
  }

  // ── STOP ───────────────────────────────────────────────────────────────────
  if (cmd == "STOP") {
    startupPhase = RUNNING; // cancel any pending sequence
    startRamp(vibRamp,      0);
    startRamp(centralRamp,  0);
    startRamp(planetaryRamp,0);
    pendingCentralRPM   = 0;
    pendingPlanetaryRPM = 0;
    pendingVibPWM       = 0;
    Serial.println("[OK] Ramping all motors down over 5 s...");
    return;
  }

  // ── STATUS ─────────────────────────────────────────────────────────────────
  if (cmd == "STATUS") {
    const char* phaseNames[] = {"IDLE","VIB_RAMP","PLANETARY_RAMP","CENTRAL_RAMP","RUNNING"};
    Serial.println("── Current Status ──────────────────────────");
    Serial.print("  Startup phase     : "); Serial.println(phaseNames[startupPhase]);
    Serial.print("  Central  target   : "); Serial.print(pendingCentralRPM);   Serial.println(" RPM");
    Serial.print("  Central  current  : "); Serial.print(centralRamp.currentVal / STEPS_PER_REV * 60); Serial.println(" RPM");
    Serial.print("  Planetary target  : "); Serial.print(pendingPlanetaryRPM); Serial.println(" RPM");
    Serial.print("  Planetary current : "); Serial.print(planetaryRamp.currentVal / STEPS_PER_REV * 60); Serial.println(" RPM");
    Serial.print("  Vib target PWM    : "); Serial.println(pendingVibPWM);
    Serial.print("  Vib current PWM   : "); Serial.println((int)vibRamp.currentVal);
    Serial.print("  Vib direction     : "); Serial.println(vibForward ? "FORWARD" : "REVERSE");
    Serial.println("────────────────────────────────────────────");
    return;
  }

  // ── C:<rpm> ────────────────────────────────────────────────────────────────
  if (cmd.startsWith("C:")) {
    float rpm = cmd.substring(2).toFloat();
    if (rpm < -600 || rpm > 600) { Serial.println("[ERR] RPM out of range (-600 to 600)"); return; }
    pendingCentralRPM = rpm;
    if (startupPhase == RUNNING) {
      startRamp(centralRamp, rpmToStepsPerSec(rpm));
      Serial.print("[OK] Central ramping to "); Serial.print(rpm); Serial.println(" RPM");
    } else {
      Serial.print("[SET] Central target stored: "); Serial.print(rpm); Serial.println(" RPM (send START to run)");
    }
    return;
  }

  // ── P:<rpm> ────────────────────────────────────────────────────────────────
  if (cmd.startsWith("P:")) {
    float rpm = cmd.substring(2).toFloat();
    if (rpm < -600 || rpm > 600) { Serial.println("[ERR] RPM out of range (-600 to 600)"); return; }
    pendingPlanetaryRPM = rpm;
    if (startupPhase == RUNNING) {
      startRamp(planetaryRamp, rpmToStepsPerSec(rpm));
      Serial.print("[OK] Planetary ramping to "); Serial.print(rpm); Serial.println(" RPM");
    } else {
      Serial.print("[SET] Planetary target stored: "); Serial.print(rpm); Serial.println(" RPM (send START to run)");
    }
    return;
  }

  // ── V:<pwm> ────────────────────────────────────────────────────────────────
  if (cmd.startsWith("V:")) {
    int spd = cmd.substring(2).toInt();
    if (spd < 0 || spd > 255) { Serial.println("[ERR] Vib speed must be 0–255"); return; }
    pendingVibPWM = spd;
    if (startupPhase == RUNNING) {
      startRamp(vibRamp, (float)spd);
      Serial.print("[OK] Vibration ramping to PWM "); Serial.println(spd);
    } else {
      Serial.print("[SET] Vib target stored: "); Serial.print(spd); Serial.println(" (send START to run)");
    }
    return;
  }

  // ── VD:<F|R> ───────────────────────────────────────────────────────────────
  if (cmd.startsWith("VD:")) {
    String dir = cmd.substring(3);
    if (dir == "F")      { vibForward = true;  Serial.println("[OK] Vib direction -> FORWARD"); }
    else if (dir == "R") { vibForward = false; Serial.println("[OK] Vib direction -> REVERSE"); }
    else                 { Serial.println("[ERR] Use VD:F or VD:R"); return; }
    setVibMotor((int)vibRamp.currentVal, vibForward);
    return;
  }

  Serial.print("[ERR] Unknown command: "); Serial.println(cmd);
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("============================================");
  Serial.println("   Surface Drag Finishing Machine v2");
  Serial.println("============================================");
  Serial.println("  1. Set speeds : C:<rpm>  P:<rpm>  V:<pwm>");
  Serial.println("  2. Send START to begin sequenced ramp-up");
  Serial.println("  Other: STOP | STATUS | VD:<F|R>");
  Serial.println("============================================");

  pinMode(VIB_RPWM, OUTPUT);
  pinMode(VIB_LPWM, OUTPUT);
  pinMode(ESTOP_PIN, INPUT_PULLUP);
  setVibMotor(0, true);

  Central_Motor.setMaxSpeed(rpmToStepsPerSec(600));
  Central_Motor.setSpeed(0);

  Planetary_Motor.setMaxSpeed(rpmToStepsPerSec(600));
  Planetary_Motor.setSpeed(0);
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
  // ── 1. Read serial ──────────────────────────────────────────────────────────
  static String inputBuffer = "";
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (inputBuffer.length() > 0) {
        parseCommand(inputBuffer);
        inputBuffer = "";
      }
    } else {
      inputBuffer += c;
    }
  }

  // ── 2. E-stop check ────────────────────────────────────────────────────────
  checkEstop();

  // ── 3. Advance startup sequence ────────────────────────────────────────────
  if (!estopActive) updateStartupSequence();

  // ── 4. Update ramp values ──────────────────────────────────────────────────
  updateRamp(vibRamp);
  updateRamp(centralRamp);
  updateRamp(planetaryRamp);

  // ── 5. Apply outputs ───────────────────────────────────────────────────────
  setVibMotor((int)vibRamp.currentVal, vibForward);

  Central_Motor.setSpeed(centralRamp.currentVal);
  Central_Motor.runSpeed();

  Planetary_Motor.setSpeed(planetaryRamp.currentVal);
  Planetary_Motor.runSpeed();

  // ── 6. Periodic status ─────────────────────────────────────────────────────
  emitStatusLine();
}