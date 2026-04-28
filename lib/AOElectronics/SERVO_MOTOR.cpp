// SERVO_MOTOR.cpp
//
// Direct LEDC implementation — no ESP32Servo library. Standard servo
// PWM: 50 Hz, pulse 500-2500 µs for full travel. 14-bit LEDC resolution
// gives ~1.2 µs pulse granularity, more than enough for any servo.
//
// arduino-esp32 API compatibility:
//   v2.x  — channel-based: ledcSetup / ledcAttachPin / ledcWrite(ch, duty)
//   v3.x  — pin-based:     ledcAttach / ledcWrite(pin, duty) / ledcDetach
// The `ledcChannel` field stores the LEDC channel in v2.x and the pin
// number in v3.x; writeServoFromParam() uses it identically in both.

#include "SERVO_MOTOR.h"
#include <Arduino.h>
#include <esp_arduino_version.h>

#if ESP_ARDUINO_VERSION_MAJOR < 3
#  include "ServoAllocator.h"
#endif

// ---------------- PWM constants ----------------

static const uint32_t SERVO_FREQ_HZ      = 50;
static const uint8_t  SERVO_RES_BITS     = 14;
static const uint32_t SERVO_PERIOD_US    = 20000;                 // 1/50 Hz
static const uint32_t SERVO_DUTY_MAX     = (1UL << SERVO_RES_BITS) - 1;
static const uint32_t SERVO_PULSE_MIN_US = 500;
static const uint32_t SERVO_PULSE_MAX_US = 2500;

static const uint32_t DEMO_PERIOD_MS     = 5000;

// ---------------- Class impl ----------------

SERVO_MOTOR::SERVO_MOTOR() {}

SERVO_MOTOR::~SERVO_MOTOR() {
    if (ledcChannel >= 0 && pin >= 0) {
        ledcWrite((uint8_t)ledcChannel, 0);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        ledcDetach(pin);
#else
        ledcDetachPin(pin);
#endif
    }
}

bool SERVO_MOTOR::configure(JsonObjectConst cfg) {
    if (!cfg["SERVO_PIN"].is<int>()) {
        Serial.println(F("# [SERVO_MOTOR] configure failed: SERVO_PIN missing or not int"));
        return false;
    }
    pin         = cfg["SERVO_PIN"].as<int>();
    minPosition = cfg["MIN_POSITION"] | 0;
    maxPosition = cfg["MAX_POSITION"] | 180;
    return true;
}

bool SERVO_MOTOR::initialise() {
    if (pin < 0) {
        Serial.println(F("# [SERVO_MOTOR] initialise: not configured"));
        return false;
    }

    // Precompute the pulse-width window corresponding to the configured
    // [minPosition, maxPosition] angle range, so the hot path write only
    // needs a single interpolation.
    pulseMinUs = SERVO_PULSE_MIN_US +
                 ((SERVO_PULSE_MAX_US - SERVO_PULSE_MIN_US) *
                  (uint32_t)minPosition) / 180;
    pulseMaxUs = SERVO_PULSE_MIN_US +
                 ((SERVO_PULSE_MAX_US - SERVO_PULSE_MIN_US) *
                  (uint32_t)maxPosition) / 180;

#if ESP_ARDUINO_VERSION_MAJOR >= 3
    // v3.x: pin-based LEDC — no channel allocation required.
    // Store pin in ledcChannel so writeServoFromParam() is unchanged.
    if (!ledcAttach((uint8_t)pin, SERVO_FREQ_HZ, SERVO_RES_BITS)) {
        Serial.printf("# [SERVO_MOTOR] ledcAttach failed on pin=%d\n", pin);
        return false;
    }
    ledcChannel = (int8_t)pin;
    Serial.printf("# [SERVO_MOTOR] pin=%d pulseWindow=%u-%uus (v3 API)\n",
                  pin, pulseMinUs, pulseMaxUs);
#else
    // v2.x: channel-based LEDC via shared allocator.
    ledcChannel = (int8_t)ServoAllocator::allocate();
    if (ledcChannel < 0) {
        Serial.println(F("# [SERVO_MOTOR] no LEDC channels available"));
        return false;
    }
    uint32_t actualFreq = ledcSetup((uint8_t)ledcChannel, SERVO_FREQ_HZ, SERVO_RES_BITS);
    if (actualFreq == 0) {
        Serial.printf("# [SERVO_MOTOR] ledcSetup failed on ch=%d\n", ledcChannel);
        return false;
    }
    ledcAttachPin(pin, (uint8_t)ledcChannel);
    Serial.printf("# [SERVO_MOTOR] pin=%d ch=%d pulseWindow=%u-%uus actualFreq=%uHz\n",
                  pin, ledcChannel, pulseMinUs, pulseMaxUs, actualFreq);
#endif

    writeServoFromParam(parameter(POSITION));
    return true;
}

void SERVO_MOTOR::allStop() {
    // A servo can't "stop" in the motor sense — it always tries to hold
    // whatever position it was commanded to. Safest interpretation: end
    // any active demo and leave the servo holding its current position.
    stopDemo();
}

void SERVO_MOTOR::updateControl(uint32_t /*dtMs*/) {
    // No ramping yet — servo's own speed limits the motion. If an art
    // piece needs software-limited sweep speed, add RAMP_TIME to schema
    // and implement here (see PWM_MOTOR for the pattern).
    writeServoFromParam(parameter(POSITION));
}

void SERVO_MOTOR::updateDemo(uint32_t dtMs) {
    demoPhaseMs = (demoPhaseMs + dtMs) % DEMO_PERIOD_MS;
    uint32_t half = DEMO_PERIOD_MS / 2;
    uint16_t v;
    if (demoPhaseMs < half) {
        v = (uint16_t)(((uint32_t)65535 * demoPhaseMs) / half);
    } else {
        v = (uint16_t)(((uint32_t)65535 * (DEMO_PERIOD_MS - demoPhaseMs)) / half);
    }
    writeServoFromParam(v);
}

void SERVO_MOTOR::onDemoStart() {
    demoPhaseMs = 0;
}

void SERVO_MOTOR::writeServoFromParam(uint16_t paramVal) {
    if (ledcChannel < 0) return;
    // Linear interpolation: param [0..65535] → pulse [pulseMinUs..pulseMaxUs].
    uint32_t pulseUs = pulseMinUs +
        (uint32_t)(((uint64_t)(pulseMaxUs - pulseMinUs) * paramVal) / 65535);
    // Pulse width → duty cycle at this resolution.
    uint32_t duty = (uint32_t)(((uint64_t)pulseUs * SERVO_DUTY_MAX) / SERVO_PERIOD_US);
    // ledcChannel stores channel (v2.x) or pin (v3.x) — ledcWrite signature
    // differs but usage is identical because v3.x uses pin where v2.x used channel.
    ledcWrite((uint8_t)ledcChannel, duty);

    // Diagnostic: log only when the commanded value changes, so we can
    // tell at a glance whether parameter updates are reaching hardware.
    if (paramVal != lastLoggedParam) {
        Serial.printf("# [SERVO_MOTOR] pin=%d param=%u pulse=%uus duty=%u\n",
                      pin, paramVal, pulseUs, duty);
        lastLoggedParam = paramVal;
    }
}
