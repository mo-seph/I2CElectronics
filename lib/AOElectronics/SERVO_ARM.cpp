// SERVO_ARM.cpp
//
// Implementation notes:
//   - Drives two LEDC channels directly (no ESP32Servo library) to match
//     SERVO_MOTOR. Channels come from the shared ServoAllocator (v2.x) so
//     mixed setups (several servos + several PWM motors) share the
//     8-channel pool cleanly. On v3.x, the framework manages channel
//     assignment automatically; ServoAllocator is not used.
//   - All the trigonometry runs in floats. Called once per loop tick
//     (~20 Hz) so it's well under any budget.
//   - Beam angle clamping happens at the writeServoAngle step — we
//     feed it the raw computed a2 and also pre-clamp in computeAngles
//     for a consistent readout.
//
// arduino-esp32 API compatibility:
//   v2.x  — channel-based: ledcSetup / ledcAttachPin / ledcWrite(ch, duty)
//   v3.x  — pin-based:     ledcAttach / ledcWrite(pin, duty) / ledcDetach
// The `baseChannel` and `upperChannel` fields store LEDC channels (v2.x)
// or pin numbers (v3.x); writeServoAngle() uses them identically in both.

#include "SERVO_ARM.h"
#include <Arduino.h>
#include <esp_arduino_version.h>

#if ESP_ARDUINO_VERSION_MAJOR < 3
#  include "ServoAllocator.h"
#endif

// ---------------- Standard servo PWM constants ----------------

static const uint32_t SERVO_FREQ_HZ      = 50;
static const uint8_t  SERVO_RES_BITS     = 14;
static const uint32_t SERVO_PERIOD_US    = 20000;
static const uint32_t SERVO_DUTY_MAX     = (1UL << SERVO_RES_BITS) - 1;
static const uint32_t SERVO_PULSE_MIN_US = 500;   // at 0°
static const uint32_t SERVO_PULSE_MAX_US = 2500;  // at 180°

// Demo periods — picked to not line up into an obvious loop.
static const uint32_t DEMO_P1_PERIOD_MS = 6000;
static const uint32_t DEMO_P2_PERIOD_MS = 10000;

// ---------------- Small helpers ----------------

static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Triangle wave in [0, 1] given a phase in [0, period).
static float trianglePhase01(uint32_t phaseMs, uint32_t periodMs) {
    if (periodMs == 0) return 0.0f;
    uint32_t half = periodMs / 2;
    if (phaseMs < half) return (float)phaseMs / (float)half;
    return (float)(periodMs - phaseMs) / (float)half;
}

// ---------------- Class impl ----------------

SERVO_ARM::SERVO_ARM() {}

SERVO_ARM::~SERVO_ARM() {
    if (baseChannel >= 0 && basePin >= 0) {
        ledcWrite((uint8_t)baseChannel, 0);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        ledcDetach(basePin);
#else
        ledcDetachPin(basePin);
#endif
    }
    if (upperChannel >= 0 && upperPin >= 0) {
        ledcWrite((uint8_t)upperChannel, 0);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        ledcDetach(upperPin);
#else
        ledcDetachPin(upperPin);
#endif
    }
}

bool SERVO_ARM::configure(JsonObjectConst cfg) {
    if (!cfg["BASE_SERVO_PIN"].is<int>() || !cfg["UPPER_SERVO_PIN"].is<int>()) {
        Serial.println(F("# [SERVO_ARM] configure failed: BASE_SERVO_PIN or UPPER_SERVO_PIN missing"));
        return false;
    }
    basePin         = cfg["BASE_SERVO_PIN"].as<int>();
    upperPin        = cfg["UPPER_SERVO_PIN"].as<int>();
    minBasePos      = cfg["MIN_BASE_POSITION"]  | 0;
    maxBasePos      = cfg["MAX_BASE_POSITION"]  | 180;
    minUpperPos     = cfg["MIN_UPPER_POSITION"] | 0;
    maxUpperPos     = cfg["MAX_UPPER_POSITION"] | 180;
    focusDistMm     = cfg["FOCUS_DISTANCE_MM"]  | 500;
    armLengthMm     = cfg["ARM_LENGTH_MM"]      | 150;
    minBeamAngleDeg = cfg["MIN_BEAM_ANGLE_DEG"] | -20;
    maxBeamAngleDeg = cfg["MAX_BEAM_ANGLE_DEG"] | 20;

    // Sanity: the kinematics need positive distances; degenerate beam
    // angle range would make parameter → φ mapping ambiguous.
    if (focusDistMm <= 0 || armLengthMm <= 0) {
        Serial.printf("# [SERVO_ARM] bad geometry: d1=%d d2=%d\n",
                      focusDistMm, armLengthMm);
        return false;
    }
    if (maxBeamAngleDeg <= minBeamAngleDeg) {
        Serial.printf("# [SERVO_ARM] bad beam angle range: [%d, %d]\n",
                      minBeamAngleDeg, maxBeamAngleDeg);
        return false;
    }
    return true;
}

bool SERVO_ARM::initialise() {
    if (basePin < 0 || upperPin < 0) {
        Serial.println(F("# [SERVO_ARM] initialise: not configured"));
        return false;
    }

#if ESP_ARDUINO_VERSION_MAJOR >= 3
    // v3.x: pin-based LEDC — no channel allocation required.
    // Store pins in the channel fields so writeServoAngle() is unchanged.
    if (!ledcAttach((uint8_t)basePin,  SERVO_FREQ_HZ, SERVO_RES_BITS) ||
        !ledcAttach((uint8_t)upperPin, SERVO_FREQ_HZ, SERVO_RES_BITS)) {
        Serial.println(F("# [SERVO_ARM] ledcAttach failed"));
        return false;
    }
    baseChannel  = (int8_t)basePin;
    upperChannel = (int8_t)upperPin;
    Serial.printf("# [SERVO_ARM] basePin=%d upperPin=%d d1=%dmm d2=%dmm beam=[%d,%d]° (v3 API)\n",
                  basePin, upperPin, focusDistMm, armLengthMm,
                  minBeamAngleDeg, maxBeamAngleDeg);
#else
    // v2.x: channel-based LEDC via shared allocator.
    baseChannel  = (int8_t)ServoAllocator::allocate();
    upperChannel = (int8_t)ServoAllocator::allocate();
    if (baseChannel < 0 || upperChannel < 0) {
        Serial.println(F("# [SERVO_ARM] not enough LEDC channels"));
        return false;
    }
    Serial.printf("# [SERVO_ARM] basePin=%d(ch%d) upperPin=%d(ch%d) d1=%dmm d2=%dmm beam=[%d,%d]°\n",
                  basePin, baseChannel, upperPin, upperChannel,
                  focusDistMm, armLengthMm, minBeamAngleDeg, maxBeamAngleDeg);
    if (ledcSetup((uint8_t)baseChannel,  SERVO_FREQ_HZ, SERVO_RES_BITS) == 0 ||
        ledcSetup((uint8_t)upperChannel, SERVO_FREQ_HZ, SERVO_RES_BITS) == 0) {
        Serial.println(F("# [SERVO_ARM] ledcSetup failed"));
        return false;
    }
    ledcAttachPin(basePin,  (uint8_t)baseChannel);
    ledcAttachPin(upperPin, (uint8_t)upperChannel);
#endif

    // Initial write at current parameter values (defaults to 0, 0 until
    // a SET_PARAMETERS arrives).
    float p1 = (float)parameter(ANGLE)  / 65535.0f;
    float p2 = (float)parameter(OFFSET) / 65535.0f;
    float a1, a2;
    computeAngles(p1, p2, a1, a2);
    writeServoAngle(baseChannel,  a1);
    writeServoAngle(upperChannel, a2);
    return true;
}

void SERVO_ARM::allStop() {
    // Servos hold their position — same convention as SERVO_MOTOR. Just
    // end any active demo so parameter changes take effect on next
    // updateControl().
    stopDemo();
}

void SERVO_ARM::updateControl(uint32_t /*dtMs*/) {
    float p1 = (float)parameter(ANGLE)  / 65535.0f;
    float p2 = (float)parameter(OFFSET) / 65535.0f;
    float a1, a2;
    computeAngles(p1, p2, a1, a2);
    writeServoAngle(baseChannel,  a1);
    writeServoAngle(upperChannel, a2);
}

void SERVO_ARM::updateDemo(uint32_t dtMs) {
    demoP1PhaseMs = (demoP1PhaseMs + dtMs) % DEMO_P1_PERIOD_MS;
    demoP2PhaseMs = (demoP2PhaseMs + dtMs) % DEMO_P2_PERIOD_MS;
    float p1 = trianglePhase01(demoP1PhaseMs, DEMO_P1_PERIOD_MS);
    float p2 = trianglePhase01(demoP2PhaseMs, DEMO_P2_PERIOD_MS);
    float a1, a2;
    computeAngles(p1, p2, a1, a2);
    writeServoAngle(baseChannel,  a1);
    writeServoAngle(upperChannel, a2);
}

void SERVO_ARM::onDemoStart() {
    demoP1PhaseMs = 0;
    demoP2PhaseMs = 0;
}

void SERVO_ARM::computeAngles(float p1Frac, float p2Frac,
                              float& a1Out, float& a2Out) const {
    // p2 → base servo angle a1 (°). Already constrained to
    // [minBasePos, maxBasePos] by construction.
    float a1 = (float)minBasePos + p2Frac * (float)(maxBasePos - minBasePos);

    // Upper servo position (world, mm). At a1=90, arm points south
    // (−y, away from wall). α = −a1 gives: U = (d2·cos(a1), −d2·sin(a1)).
    float a1Rad = a1 * (float)DEG_TO_RAD;
    float Ux =  (float)armLengthMm * cosf(a1Rad);
    float Uy = -(float)armLengthMm * sinf(a1Rad);

    // p1 → target beam angle φ (°) on the wall.
    float phiDeg = (float)minBeamAngleDeg +
                   p1Frac * (float)(maxBeamAngleDeg - minBeamAngleDeg);
    float phiRad = phiDeg * (float)DEG_TO_RAD;

    // Target point on the wall (wall at y = d1).
    float Tx = (float)focusDistMm * tanf(phiRad);
    float Ty = (float)focusDistMm;

    // Required beam direction, inverted to upper-servo command.
    // β = −a1 + a2 + 90  ⟹  a2 = β + a1 − 90
    float betaRad = atan2f(Ty - Uy, Tx - Ux);
    float betaDeg = betaRad * (float)RAD_TO_DEG;
    float a2 = betaDeg + a1 - 90.0f;

    // Clamp a2 to the upper servo's safe mechanical range.
    a2 = clampf(a2, (float)minUpperPos, (float)maxUpperPos);

    a1Out = a1;
    a2Out = a2;
}

void SERVO_ARM::writeServoAngle(int8_t pinOrChannel, float angleDeg) const {
    if (pinOrChannel < 0) return;
    angleDeg = clampf(angleDeg, 0.0f, 180.0f);
    // Linear map: angle [0, 180] → pulse [500, 2500] µs.
    uint32_t pulseUs = SERVO_PULSE_MIN_US +
        (uint32_t)((float)(SERVO_PULSE_MAX_US - SERVO_PULSE_MIN_US) * angleDeg / 180.0f);
    uint32_t duty = (uint32_t)(((uint64_t)pulseUs * SERVO_DUTY_MAX) / SERVO_PERIOD_US);
    // pinOrChannel stores channel (v2.x) or pin (v3.x) — ledcWrite works
    // identically because v3.x uses pin where v2.x used channel.
    ledcWrite((uint8_t)pinOrChannel, duty);
}
