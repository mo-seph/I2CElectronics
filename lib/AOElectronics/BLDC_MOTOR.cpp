// BLDC_MOTOR.cpp
//
// Direct LEDC driver for BLHeli-style ESCs.  See BLDC_MOTOR.h for the
// behaviour summary and config-field meanings.
//
// arduino-esp32 API compatibility:
//   v2.x  — channel-based: ledcSetup / ledcAttachPin / ledcWrite(ch, duty)
//   v3.x  — pin-based:     ledcAttach / ledcWrite(pin, duty) / ledcDetach
// `ledcChannel` stores the LEDC channel in v2.x and the pin number in
// v3.x; writeFromCurrent() uses it identically in both — matches the
// pattern in SERVO_MOTOR.cpp / PWM_MOTOR.cpp.

#include "BLDC_MOTOR.h"
#include <Arduino.h>
#include <esp_arduino_version.h>

#if ESP_ARDUINO_VERSION_MAJOR < 3
#  include "ServoAllocator.h"
#endif

// 14-bit duty resolution. arduino-esp32 v2.x caps LEDC at 14 bits (the
// ESP32-S3 hardware can do up to 20 at 50 Hz, but the Arduino layer
// rejects anything wider). At 50 Hz that's ~1.22 µs per LSB, ~820 LSBs
// across the 1000-2000 µs throttle range — plenty for an ESC.
// Matches the resolution SERVO_MOTOR uses.
static const uint8_t  BLDC_RES_BITS  = 14;
static const uint32_t BLDC_DUTY_MAX  = (1UL << BLDC_RES_BITS) - 1;

// Demo: slow gentle triangle so it's clear something's working without
// being scary. 0 → 20% throttle → 0 over DEMO_PERIOD_MS. The user can
// drive harder via the slider once they've verified the rig.
static const uint32_t DEMO_PERIOD_MS = 8000;
static const uint16_t DEMO_PEAK_PARAM = 13107; // 0.2 * 65535

BLDC_MOTOR::BLDC_MOTOR() {}

BLDC_MOTOR::~BLDC_MOTOR() {
    if (ledcChannel >= 0 && pin >= 0) {
        ledcWrite((uint8_t)ledcChannel, 0);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        ledcDetach(pin);
#else
        ledcDetachPin(pin);
#endif
    }
}

bool BLDC_MOTOR::configure(JsonObjectConst cfg) {
    if (!cfg["ESC_PIN"].is<int>()) {
        Serial.println(F("# [BLDC_MOTOR] configure failed: ESC_PIN missing or not int"));
        return false;
    }
    pin            = cfg["ESC_PIN"].as<int>();
    minPulseUs     = cfg["MIN_PULSE_US"]    | 1000;
    maxPulseUs     = cfg["MAX_PULSE_US"]    | 2000;
    updateFreqHz   = cfg["UPDATE_FREQ_HZ"]  | 50;
    armDurationMs  = cfg["ARM_DURATION_MS"] | 2000;
    rampTimeMs     = cfg["RAMP_TIME"]       | 200;

    // Sanity: an ESC won't arm if MAX <= MIN. Reject early so the user
    // sees a clear error rather than a motor that "doesn't respond".
    if (maxPulseUs <= minPulseUs) {
        Serial.printf("# [BLDC_MOTOR] configure failed: MAX_PULSE_US (%u) "
                      "must be > MIN_PULSE_US (%u)\n",
                      (unsigned)maxPulseUs, (unsigned)minPulseUs);
        return false;
    }
    if (updateFreqHz == 0) {
        Serial.println(F("# [BLDC_MOTOR] configure failed: UPDATE_FREQ_HZ = 0"));
        return false;
    }
    return true;
}

bool BLDC_MOTOR::initialise() {
    if (pin < 0) {
        Serial.println(F("# [BLDC_MOTOR] initialise: not configured"));
        return false;
    }

    periodUs = 1000000UL / updateFreqHz;
    dutyMax  = BLDC_DUTY_MAX;

#if ESP_ARDUINO_VERSION_MAJOR >= 3
    if (!ledcAttach((uint8_t)pin, updateFreqHz, BLDC_RES_BITS)) {
        Serial.printf("# [BLDC_MOTOR] ledcAttach failed (pin=%d freq=%uHz)\n",
                      pin, (unsigned)updateFreqHz);
        return false;
    }
    ledcChannel = (int8_t)pin;
    Serial.printf("# [BLDC_MOTOR] pin=%d freq=%uHz period=%uus (v3 API)\n",
                  pin, (unsigned)updateFreqHz, (unsigned)periodUs);
#else
    // v2.x: share the LEDC pool with SERVO_MOTOR / SERVO_ARM via the
    // top-down allocator. ESCs run at servo-like frequencies anyway.
    ledcChannel = (int8_t)ServoAllocator::allocate();
    if (ledcChannel < 0) {
        Serial.println(F("# [BLDC_MOTOR] no LEDC channels available"));
        return false;
    }
    uint32_t actualFreq = ledcSetup((uint8_t)ledcChannel, updateFreqHz, BLDC_RES_BITS);
    if (actualFreq == 0) {
        Serial.printf("# [BLDC_MOTOR] ledcSetup failed (ch=%d freq=%uHz res=%u)\n",
                      ledcChannel, (unsigned)updateFreqHz, BLDC_RES_BITS);
        return false;
    }
    ledcAttachPin(pin, (uint8_t)ledcChannel);
    Serial.printf("# [BLDC_MOTOR] pin=%d ch=%d freq=%uHz (actual %u) period=%uus\n",
                  pin, ledcChannel, (unsigned)updateFreqHz,
                  (unsigned)actualFreq, (unsigned)periodUs);
#endif

    // Start emitting MIN_PULSE — the ESC's "armed but at zero throttle"
    // signal. The duty stays on the pin until updateControl writes again.
    currentParam = 0;
    writeFromCurrent();

    // Block for the configured arming window so the ESC sees a sustained
    // MIN_PULSE before any subsequent commands. This is the simple,
    // intended-to-be-temporary approach: with N ESCs on one board, total
    // boot time grows by ~armDurationMs × N. See WORKING.md for the plan
    // to parallelise this across all Elements.
    Serial.printf("# [BLDC_MOTOR] pin=%d arming (delay %ums)...\n",
                  pin, (unsigned)armDurationMs);
    delay(armDurationMs);
    Serial.printf("# [BLDC_MOTOR] pin=%d armed\n", pin);

    return true;
}

void BLDC_MOTOR::allStop() {
    stopDemo();
    _params[THROTTLE] = 0;
    currentParam = 0;
    writeFromCurrent();
}

void BLDC_MOTOR::updateControl(uint32_t dtMs) {
    rampAndWrite(dtMs);
}

void BLDC_MOTOR::updateDemo(uint32_t dtMs) {
    demoPhaseMs = (demoPhaseMs + dtMs) % DEMO_PERIOD_MS;
    uint32_t half = DEMO_PERIOD_MS / 2;
    uint16_t v;
    if (demoPhaseMs < half) {
        v = (uint16_t)(((uint32_t)DEMO_PEAK_PARAM * demoPhaseMs) / half);
    } else {
        v = (uint16_t)(((uint32_t)DEMO_PEAK_PARAM * (DEMO_PERIOD_MS - demoPhaseMs)) / half);
    }
    // Demo writes directly through the ramp so a switch into demo mode
    // doesn't sidestep the soft-start protection. Same shape as PWM_MOTOR
    // but using the ramp path here as well — brushless motors don't like
    // sudden throttle steps.
    currentParam = v;
    writeFromCurrent();
}

void BLDC_MOTOR::onDemoStart() {
    demoPhaseMs = 0;
}

void BLDC_MOTOR::rampAndWrite(uint32_t dtMs) {
    uint16_t target = parameter(THROTTLE);
    if (currentParam == target) { writeFromCurrent(); return; }
    if (rampTimeMs == 0) {
        currentParam = target;
    } else {
        uint32_t maxStep = ((uint32_t)65535 * dtMs) / rampTimeMs;
        int32_t diff = (int32_t)target - (int32_t)currentParam;
        if ((uint32_t)abs(diff) <= maxStep) {
            currentParam = target;
        } else if (diff > 0) {
            currentParam += (uint16_t)maxStep;
        } else {
            currentParam -= (uint16_t)maxStep;
        }
    }
    writeFromCurrent();
}

void BLDC_MOTOR::writeFromCurrent() {
    if (ledcChannel < 0) return;

    // Linear interpolation: currentParam [0..65535] → pulse [minPulseUs..maxPulseUs].
    uint32_t pulseUs = minPulseUs +
        (uint32_t)(((uint64_t)(maxPulseUs - minPulseUs) * currentParam) / 65535);

    // Pulse width → duty cycle at this resolution.
    uint32_t duty = (uint32_t)(((uint64_t)pulseUs * dutyMax) / periodUs);
    if (duty > dutyMax) duty = dutyMax;

    ledcWrite((uint8_t)ledcChannel, duty);

    // Change-only diagnostic log.
    if (currentParam != lastLoggedParam) {
        Serial.printf("# [BLDC_MOTOR] pin=%d param=%u pulse=%uus duty=%u\n",
                      pin, currentParam, (unsigned)pulseUs, (unsigned)duty);
        lastLoggedParam = currentParam;
    }
}
