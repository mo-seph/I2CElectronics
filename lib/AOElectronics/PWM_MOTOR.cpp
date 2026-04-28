// PWM_MOTOR.cpp
//
// Direct LEDC implementation. 20 kHz default frequency (above audible,
// within most H-bridge driver specs). 8-bit resolution = duty 0..255.
//
// arduino-esp32 API compatibility:
//   v2.x  — channel-based: ledcSetup / ledcAttachPin / ledcWrite(ch, duty)
//   v3.x  — pin-based:     ledcAttach / ledcWrite(pin, duty) / ledcDetach
// The `ledcChannel` field stores the LEDC channel in v2.x and the pin
// number in v3.x; writePwmFromCurrent() uses it identically in both.
//
// Channel allocation (v2.x only): starts at 0 and counts up.
// SERVO_MOTOR / SERVO_ARM allocate from 7 down via ServoAllocator.
// So a single board can mix up to 8 LEDC-using Elements total.
// ESP32-C6 has 6 LEDC channels max (vs 8 on S3).

#include "PWM_MOTOR.h"
#include <Arduino.h>
#include <esp_arduino_version.h>

static const uint8_t  PWM_RES_BITS   = 8;
static const uint32_t DEMO_PERIOD_MS = 4000;

// v2.x only: static channel allocator — counts up.
#if ESP_ARDUINO_VERSION_MAJOR < 3
static int8_t g_nextPwmChannel = 0;
static const int8_t PWM_CHANNEL_MAX = 7;
#endif

PWM_MOTOR::PWM_MOTOR() {}

PWM_MOTOR::~PWM_MOTOR() {
    if (ledcChannel >= 0 && pin >= 0) {
        ledcWrite((uint8_t)ledcChannel, 0);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        ledcDetach(pin);
#else
        ledcDetachPin(pin);
#endif
    }
}

bool PWM_MOTOR::configure(JsonObjectConst cfg) {
    if (!cfg["PWM_PIN"].is<int>()) {
        Serial.println(F("# [PWM_MOTOR] configure failed: PWM_PIN missing or not int"));
        return false;
    }
    pin        = cfg["PWM_PIN"].as<int>();
    minSpeed   = cfg["MIN_SPEED"] | 0;
    maxSpeed   = cfg["MAX_SPEED"] | 255;
    pwmFreqHz  = cfg["PWM_FREQ"]  | 20000;
    rampTimeMs = cfg["RAMP_TIME"] | 100;
    invert     = cfg["INVERT"]    | false;
    return true;
}

bool PWM_MOTOR::initialise() {
    if (pin < 0) {
        Serial.println(F("# [PWM_MOTOR] initialise: not configured"));
        return false;
    }

#if ESP_ARDUINO_VERSION_MAJOR >= 3
    // v3.x: pin-based LEDC — no channel allocation required.
    // Store pin in ledcChannel so writePwmFromCurrent() is unchanged.
    if (!ledcAttach((uint8_t)pin, pwmFreqHz, PWM_RES_BITS)) {
        Serial.printf("# [PWM_MOTOR] ledcAttach failed (pin=%d freq=%uHz)\n",
                      pin, (unsigned)pwmFreqHz);
        return false;
    }
    ledcChannel = (int8_t)pin;
    Serial.printf("# [PWM_MOTOR] pin=%d freq=%uHz (v3 API)\n",
                  pin, (unsigned)pwmFreqHz);
#else
    // v2.x: channel-based LEDC.
    if (g_nextPwmChannel > PWM_CHANNEL_MAX) {
        Serial.println(F("# [PWM_MOTOR] no LEDC channels available"));
        return false;
    }
    ledcChannel = g_nextPwmChannel++;
    uint32_t actualFreq = ledcSetup((uint8_t)ledcChannel, pwmFreqHz, PWM_RES_BITS);
    if (actualFreq == 0) {
        Serial.printf("# [PWM_MOTOR] ledcSetup failed (ch=%d freq=%uHz)\n",
                      ledcChannel, (unsigned)pwmFreqHz);
        return false;
    }
    Serial.printf("# [PWM_MOTOR] pin=%d ch=%d freq=%uHz (actual %u)\n",
                  pin, ledcChannel, (unsigned)pwmFreqHz, (unsigned)actualFreq);
    ledcAttachPin(pin, (uint8_t)ledcChannel);
#endif

    writePwmFromCurrent(); // initial duty = 0
    return true;
}

void PWM_MOTOR::allStop() {
    stopDemo();
    _params[SPEED] = 0;
    currentParam = 0;
    writePwmFromCurrent();
}

void PWM_MOTOR::updateControl(uint32_t dtMs) {
    rampAndWrite(dtMs);
}

void PWM_MOTOR::updateDemo(uint32_t dtMs) {
    demoPhaseMs = (demoPhaseMs + dtMs) % DEMO_PERIOD_MS;
    uint32_t half = DEMO_PERIOD_MS / 2;
    uint16_t v;
    if (demoPhaseMs < half) {
        v = (uint16_t)(((uint32_t)65535 * demoPhaseMs) / half);
    } else {
        v = (uint16_t)(((uint32_t)65535 * (DEMO_PERIOD_MS - demoPhaseMs)) / half);
    }
    currentParam = v;
    writePwmFromCurrent();
}

void PWM_MOTOR::onDemoStart() {
    demoPhaseMs = 0;
}

void PWM_MOTOR::rampAndWrite(uint32_t dtMs) {
    uint16_t target = parameter(SPEED);
    if (currentParam == target) return;
    if (rampTimeMs == 0) {
        currentParam = target;
    } else {
        uint32_t maxStep = (65535UL * dtMs) / rampTimeMs;
        int32_t diff = (int32_t)target - (int32_t)currentParam;
        if ((uint32_t)abs(diff) <= maxStep) {
            currentParam = target;
        } else if (diff > 0) {
            currentParam += (uint16_t)maxStep;
        } else {
            currentParam -= (uint16_t)maxStep;
        }
    }
    writePwmFromCurrent();
}

void PWM_MOTOR::writePwmFromCurrent() {
    if (ledcChannel < 0) return;
    int32_t range = (int32_t)maxSpeed - (int32_t)minSpeed;
    int32_t duty  = minSpeed + (int32_t)(((int64_t)range * currentParam) / 65535);
    if (duty < 0) duty = 0;
    if (duty > 255) duty = 255;
    // Pin polarity flip — for active-low drivers (e.g. high-side P-channel
    // FET) the motor sees full power when the pin is *low*, so the LEDC
    // duty needs to be the complement of what we computed.
    if (invert) duty = 255 - duty;
    // ledcChannel stores the LEDC channel (v2.x) or pin (v3.x) — the
    // ledcWrite signature differs between versions but the field usage is
    // identical because v3.x uses pin where v2.x used channel.
    ledcWrite((uint8_t)ledcChannel, (uint32_t)duty);
}
