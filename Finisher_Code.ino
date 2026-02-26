#include <AccelStepper.h>

// ---------------- PIN SETTINGS ----------------
const int PLANET_STEP = 4;
const int PLANET_DIR  = 3;
const int PLANET_EN   = 2;

const int CENTRAL_STEP = 6;
const int CENTRAL_DIR  = 5;
const int CENTRAL_EN   = 7;

// ---------------- STEPPER CONFIG ----------------
const int STEPS_PER_REV = 200;

// ---------------- DEFAULT PARAMETERS ----------------
float planetRPM  = 0.0f;
float centralRPM = 0.0f

float defaultAccelRPMs2 = 50.0f;

// ---------------- SOP SETTINGS ----------------
const float SOP_RAMP_TIME_SEC       = 15.0f;   // initial ramp-up
const float SOP_PLANET_TARGET_RPM   = 170.0f;
const float SOP_CENTRAL_TARGET_RPM  = 150.0f;

// Reversal ramp settings
const float REV_DECEL_TIME_SEC      = 15.0f;
const float REV_ACCEL_TIME_SEC      = 15.0f;
const float REV_TOTAL_TIME_SEC      = REV_DECEL_TIME_SEC + REV_ACCEL_TIME_SEC;

// Pause behavior
const float PAUSE_DECEL_TIME_SEC    = 3.0f;

// ---------------- STATE ----------------
AccelStepper planet(AccelStepper::DRIVER, PLANET_STEP, PLANET_DIR);
AccelStepper central(AccelStepper::DRIVER, CENTRAL_STEP, CENTRAL_DIR);

bool systemRunning = false;
bool sopActive     = false;
unsigned long sopStartMs = 0;
bool sopRampDone  = false;

// ---------------- MANUAL RAMP STATE ----------------
bool   rampPlanetActive  = false;
bool   rampCentralActive = false;
float  p_startRPM = 0, p_targetRPM = 0;
float  c_startRPM = 0, c_targetRPM = 0;
unsigned long p_rampStartMs = 0;
unsigned long c_rampStartMs = 0;

const float MANUAL_RAMP_TIME_SEC = 5.0f;

// ---------------- SOP REVERSAL STATE ----------------
long centralStepCounter = 0;
long lastCentralPos     = 0;
int  centralDirection   = 1;           // 1 = forward, -1 = reverse
const long CENTRAL_REVERSAL_STEPS = 800L * STEPS_PER_REV;

bool   reversalActive   = false;       // true during the 30-second decel+accel
bool   reversalFlipped  = false;       // true after direction flipped at 0 RPM
unsigned long reversalStartMs = 0;

// ---------------- SOP PAUSE STATE ----------------
bool sopPaused           = false;
unsigned long pauseStartMs = 0;
float pausePlanetStartRPM  = 0.0f;
float pauseCentralStartRPM = 0.0f;

// ---------------- HELPERS ----------------
float rpmToStepsPerSec(float rpm) {
  return (rpm * STEPS_PER_REV) / 60.0f;
}

float rpm2ToStepsPerSec2(float rpm_s2) {
  return (rpm_s2 * STEPS_PER_REV) / 60.0f;
}

void setPlanetProfile(float rpm) {
  float spd = rpmToStepsPerSec(rpm);
  float acc = rpm2ToStepsPerSec2(defaultAccelRPMs2);
  planet.setMaxSpeed(spd);
  planet.setAcceleration(acc);
  planet.setSpeed(spd);
}

void setCentralProfile(float rpm) {
  float spd = rpmToStepsPerSec(rpm);
  float acc = rpm2ToStepsPerSec2(defaultAccelRPMs2);
  central.setMaxSpeed(spd);
  central.setAcceleration(acc);
  central.setSpeed(spd * centralDirection);
}

void applySpeeds() {
  setPlanetProfile(planetRPM);
  setCentralProfile(centralRPM);
}

// ---------------- SOP INITIAL RAMP ----------------
void updateSopInitialRamp() {
  if (sopRampDone) return;

  float elapsedSec = (millis() - sopStartMs) / 1000.0f;

  if (elapsedSec >= SOP_RAMP_TIME_SEC) {
    planetRPM  = SOP_PLANET_TARGET_RPM;
    centralRPM = SOP_CENTRAL_TARGET_RPM;
    applySpeeds();
    sopRampDone = true;
    Serial.println("[SOP] Initial 15s ramp complete.");
    return;
  }

  float r = elapsedSec / SOP_RAMP_TIME_SEC;   // 0 → 1

  planetRPM  = r * SOP_PLANET_TARGET_RPM;
  centralRPM = r * SOP_CENTRAL_TARGET_RPM;

  applySpeeds();
}

// ---------------- MANUAL RAMP ----------------
void updateManualRamps() {
  unsigned long now = millis();

  if (rampPlanetActive) {
    float ratio = (now - p_rampStartMs) / 1000.0f / MANUAL_RAMP_TIME_SEC;
    if (ratio >= 1.0f) {
      rampPlanetActive = false;
      planetRPM = p_targetRPM;
    } else {
      planetRPM = p_startRPM + (p_targetRPM - p_startRPM) * ratio;
    }
    setPlanetProfile(planetRPM);
  }

  if (rampCentralActive) {
    float ratio = (now - c_rampStartMs) / 1000.0f / MANUAL_RAMP_TIME_SEC;
    if (ratio >= 1.0f) {
      rampCentralActive = false;
      centralRPM = c_targetRPM;
    } else {
      centralRPM = c_startRPM + (c_targetRPM - c_startRPM) * ratio;
    }
    setCentralProfile(centralRPM);
  }
}

// ---------------- SOP REVERSAL PROFILE ----------------
void startReversalSequence() {
  if (!sopActive || !sopRampDone || reversalActive) return;

  reversalActive  = true;
  reversalFlipped = false;
  reversalStartMs = millis();

  // reset step counter for the next 800 rev after this reversal completes
  centralStepCounter = 0;

  Serial.println("[SOP] Reversal sequence start (15s decel + 15s accel).");
}

void updateReversalSequence() {
  if (!reversalActive) return;

  float t = (millis() - reversalStartMs) / 1000.0f;

  if (t <= REV_DECEL_TIME_SEC) {
    // Phase 1: decelerate from full SOP speed to 0
    float r_dec = t / REV_DECEL_TIME_SEC;       // 0→1
    float factor = 1.0f - r_dec;                // 1→0

    planetRPM  = SOP_PLANET_TARGET_RPM  * factor;
    centralRPM = SOP_CENTRAL_TARGET_RPM * factor;
    applySpeeds();
  }
  else if (t <= REV_TOTAL_TIME_SEC) {
    // Flip direction only once, right at the start of accel phase
    if (!reversalFlipped) {
      centralDirection *= -1;
      reversalFlipped = true;
      Serial.println("[SOP] Central direction reversed.");
    }

    float t_acc = t - REV_DECEL_TIME_SEC;       // 0→15
    float r_acc = t_acc / REV_ACCEL_TIME_SEC;   // 0→1

    planetRPM  = SOP_PLANET_TARGET_RPM  * r_acc;
    centralRPM = SOP_CENTRAL_TARGET_RPM * r_acc;
    applySpeeds();
  }
  else {
    // Reversal sequence complete, back at full SOP speeds
    reversalActive  = false;
    reversalFlipped = false;

    planetRPM  = SOP_PLANET_TARGET_RPM;
    centralRPM = SOP_CENTRAL_TARGET_RPM;
    applySpeeds();

    // Restart step-counting from here
    lastCentralPos = central.currentPosition();

    Serial.println("[SOP] Reversal sequence complete, back at full speed.");
  }
}

// ---------------- SOP PAUSE HANDLING ----------------
void startPause() {
  if (!sopActive || sopPaused) return;

  sopPaused = true;
  pauseStartMs = millis();
  pausePlanetStartRPM  = planetRPM;
  pauseCentralStartRPM = centralRPM;

  Serial.println("[SOP] Pause start.");
}

void resumePause() {
  if (!sopPaused) return;

  // Adjust SOP time references so SOP timing excludes pause duration
  unsigned long delta = millis() - pauseStartMs;

  sopStartMs += delta;
  if (reversalActive) {
    reversalStartMs += delta;
  }

  sopPaused = false;
  Serial.println("[SOP] Pause resume.");
}

void updatePause() {
  if (!sopPaused) return;

  float t = (millis() - pauseStartMs) / 1000.0f;

  if (t <= PAUSE_DECEL_TIME_SEC) {
    float r_dec = t / PAUSE_DECEL_TIME_SEC;   // 0→1
    float factor = 1.0f - r_dec;             // 1→0

    planetRPM  = pausePlanetStartRPM  * factor;
    centralRPM = pauseCentralStartRPM * factor;
    applySpeeds();
  } else {
    planetRPM  = 0.0f;
    centralRPM = 0.0f;
    applySpeeds();
  }
}

// ---------------- SERIAL COMMANDS ----------------
void handleSerialCommand() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd.length() == 0) return;

  String upper = cmd;
  upper.toUpperCase();

  // ---- SOP ----
  if (upper == "SOP") {
    systemRunning = false;
    sopActive     = true;
    sopRampDone   = false;

    rampPlanetActive  = false;
    rampCentralActive = false;
    reversalActive    = false;
    reversalFlipped   = false;
    sopPaused         = false;

    planetRPM  = 0.0f;
    centralRPM = 0.0f;

    centralDirection   = 1;
    centralStepCounter = 0;
    lastCentralPos     = central.currentPosition();

    applySpeeds();

    sopStartMs    = millis();
    systemRunning = true;

    Serial.println("[CMD] SOP start (15s ramp to 170/150, smooth reversals every 800 central revs).");
    return;
  }

  // ---- START (manual mode) ----
  if (upper == "START" || upper == "S") {
    sopActive        = false;
    sopRampDone      = false;
    reversalActive   = false;
    sopPaused        = false;
    systemRunning    = true;
    applySpeeds();
    Serial.println("[CMD] START");
    return;
  }

  // ---- STOP ----
  if (upper == "STOP" || upper == "X") {
    systemRunning     = false;
    sopActive         = false;
    sopRampDone       = false;
    rampPlanetActive  = false;
    rampCentralActive = false;
    reversalActive    = false;
    reversalFlipped   = false;
    sopPaused         = false;

    planet.setSpeed(0);
    central.setSpeed(0);

    Serial.println("[CMD] STOP");
    return;
  }

  // ---- PAUSE / RESUME SOP ----
  if (upper == "PAUSE") {
    if (sopActive && !sopPaused) {
      startPause();
    } else {
      Serial.println("[SOP] Pause ignored (not in SOP or already paused).");
    }
    return;
  }

  if (upper == "RESUME") {
    if (sopActive && sopPaused) {
      resumePause();
    } else {
      Serial.println("[SOP] Resume ignored (not paused).");
    }
    return;
  }

  // ---- MANUAL REVERSE (full 30s sequence) ----
  if (upper == "REV") {
    if (sopActive && sopRampDone && !reversalActive && !sopPaused) {
      startReversalSequence();
      Serial.println("[SOP] Manual reversal requested.");
    } else {
      Serial.println("[SOP] Manual reversal ignored (SOP inactive, ramp not done, already reversing, or paused).");
    }
    return;
  }

  // Cancel SOP on any manual speed command
  if (sopActive && !upper.startsWith("P ") && !upper.startsWith("C ")) {
    // keep SOP active unless it's clearly "manual mode" command? We'll
    // only cancel SOP on explicit manual speed commands below.
  }

  // ---------- Planet UP/DOWN ----------
  if (upper == "P UP") {
    if (sopActive) {
      Serial.println("[SOP] Cancelled by manual P UP.");
      sopActive       = false;
      sopRampDone     = false;
      reversalActive  = false;
      sopPaused       = false;
    }
    p_startRPM       = planetRPM;
    p_targetRPM      = planetRPM + 10.0f;
    rampPlanetActive = true;
    p_rampStartMs    = millis();
    Serial.print("[CMD] P UP → "); Serial.println(p_targetRPM);
    return;
  }

  if (upper == "P DOWN") {
    if (sopActive) {
      Serial.println("[SOP] Cancelled by manual P DOWN.");
      sopActive       = false;
      sopRampDone     = false;
      reversalActive  = false;
      sopPaused       = false;
    }
    p_startRPM       = planetRPM;
    p_targetRPM      = max(0.0f, planetRPM - 10.0f);
    rampPlanetActive = true;
    p_rampStartMs    = millis();
    Serial.print("[CMD] P DOWN → "); Serial.println(p_targetRPM);
    return;
  }

  // ---------- Central UP/DOWN ----------
  if (upper == "C UP") {
    if (sopActive) {
      Serial.println("[SOP] Cancelled by manual C UP.");
      sopActive       = false;
      sopRampDone     = false;
      reversalActive  = false;
      sopPaused       = false;
    }
    c_startRPM         = centralRPM;
    c_targetRPM        = centralRPM + 10.0f;
    rampCentralActive  = true;
    c_rampStartMs      = millis();
    Serial.print("[CMD] C UP → "); Serial.println(c_targetRPM);
    return;
  }

  if (upper == "C DOWN") {
    if (sopActive) {
      Serial.println("[SOP] Cancelled by manual C DOWN.");
      sopActive       = false;
      sopRampDone     = false;
      reversalActive  = false;
      sopPaused       = false;
    }
    c_startRPM         = centralRPM;
    c_targetRPM        = max(0.0f, centralRPM - 10.0f);
    rampCentralActive  = true;
    c_rampStartMs      = millis();
    Serial.print("[CMD] C DOWN → "); Serial.println(c_targetRPM);
    return;
  }

  // ---------- P <rpm> ----------
  if (upper.startsWith("P ")) {
    float rpm          = cmd.substring(2).toFloat();
    if (sopActive) {
      Serial.println("[SOP] Cancelled by manual P <rpm>.");
      sopActive       = false;
      sopRampDone     = false;
      reversalActive  = false;
      sopPaused       = false;
    }
    p_startRPM         = planetRPM;
    p_targetRPM        = rpm;
    rampPlanetActive   = true;
    p_rampStartMs      = millis();
    Serial.print("[CMD] Planet ramp → "); Serial.println(rpm);
    return;
  }

  // ---------- C <rpm> ----------
  if (upper.startsWith("C ")) {
    float rpm          = cmd.substring(2).toFloat();
    if (sopActive) {
      Serial.println("[SOP] Cancelled by manual C <rpm>.");
      sopActive       = false;
      sopRampDone     = false;
      reversalActive  = false;
      sopPaused       = false;
    }
    c_startRPM         = centralRPM;
    c_targetRPM        = rpm;
    rampCentralActive  = true;
    c_rampStartMs      = millis();
    Serial.print("[CMD] Central ramp → "); Serial.println(rpm);
    return;
  }

  Serial.print("[ERR] Unknown cmd: ");
  Serial.println(cmd);
}

// ---------------- MAIN ----------------
void setup() {
  Serial.begin(115200);

  pinMode(PLANET_EN, OUTPUT);
  pinMode(CENTRAL_EN, OUTPUT);
  digitalWrite(PLANET_EN, LOW);
  digitalWrite(CENTRAL_EN, LOW);

  float acc = rpm2ToStepsPerSec2(defaultAccelRPMs2);
  planet.setAcceleration(acc);
  central.setAcceleration(acc);

  applySpeeds();

  Serial.println("Auto Surface Ready (SOP: 15s ramp, smooth 30s reversals every 800 central revs, PAUSE+REV supported).");
}

void loop() {
  handleSerialCommand();

  if (!systemRunning) {
    planet.runSpeed();
    central.runSpeed();
    return;
  }

  // If SOP is paused, override with pause decel/hold behavior
  if (sopActive && sopPaused) {
    updatePause();
  }
  else if (sopActive) {
    // 1) Initial SOP ramp if not finished
    if (!sopRampDone) {
      updateSopInitialRamp();
    }
    // 2) Reversal sequence once SOP ramp is done
    else if (reversalActive) {
      updateReversalSequence();
    }
    // 3) Full-speed SOP, counting 800 central revs to trigger reversal
    else {
      long currentPos = central.currentPosition();
      long delta      = labs(currentPos - lastCentralPos);
      lastCentralPos  = currentPos;

      centralStepCounter += delta;

      if (centralStepCounter >= CENTRAL_REVERSAL_STEPS) {
        startReversalSequence();
      }

      // keep full speeds
      planetRPM  = SOP_PLANET_TARGET_RPM;
      centralRPM = SOP_CENTRAL_TARGET_RPM;
      applySpeeds();
    }
  }
  else {
    // Manual mode
    updateManualRamps();
  }

  planet.runSpeed();
  central.runSpeed();
}
