// PWM_MOTOR.h
//
// A DC motor driven by a PWM pin. Parameter 0 (SPEED) maps to PWM duty
// between MIN_SPEED and MAX_SPEED config values. Changes are ramped over
// RAMP_TIME ms to avoid sudden jolts.
//
// Demo mode: continuous triangle-wave sweep between 0 and full speed,
// period ~4s, until stopDemo() is called.
//
// Implementation: uses arduino-esp32's LEDC API directly (ledcSetup /
// ledcAttachPin / ledcWrite). Default frequency is 20 kHz — above the
// audible range (so the motor doesn't whine) and within the spec of
// typical H-bridge drivers (DRV8871, TB6612, L298 etc.). Override via
// the PWM_FREQ config field. LEDC channels allocated from 0 upward;
// SERVO_MOTOR allocates from 7 downward, so a board can mix up to 8
// total elements that use LEDC.

#pragma once

#include "Element.h"
#include "Protocol.h"

class PWM_MOTOR : public Element {
public:
    // Named parameter indices — so `parameter(SPEED)` reads more clearly
    // than `parameter(0)`.
    enum Param : uint8_t { SPEED = 0 };

    PWM_MOTOR();
    virtual ~PWM_MOTOR() override;

    bool configure(JsonObjectConst cfg) override;
    bool initialise() override;
    void allStop() override;

    uint8_t parameterCount() const override { return 1; }
    uint8_t typeId() const override { return AOProtocol::TYPE_PWM_MOTOR; }
    const char* typeName() const override { return "PWM_MOTOR"; }

protected:
    void updateControl(uint32_t dtMs) override;
    void updateDemo(uint32_t dtMs) override;
    void onDemoStart() override;

private:
    // Config values (from config.json).
    int pin = -1;
    int minSpeed = 0;       // PWM duty at SPEED = 0.0
    int maxSpeed = 255;     // PWM duty at SPEED = 1.0
    uint32_t pwmFreqHz = 20000;
    uint32_t rampTimeMs = 100;
    bool invert = false;    // false = active-high (pin pulled high = motor on),
                            // true  = active-low  (pin pulled low  = motor on)

    // The currently-applied parameter value (ramps toward parameter(SPEED)).
    uint16_t currentParam = 0;

    // LEDC state.
    int8_t ledcChannel = -1;

    // Demo state: phase accumulator in ms, wraps on DEMO_PERIOD_MS.
    uint32_t demoPhaseMs = 0;

    // Ramp currentParam toward parameter(SPEED) over dtMs, then write.
    void rampAndWrite(uint32_t dtMs);

    // Map currentParam [0,65535] to PWM duty [minSpeed,maxSpeed] and write.
    void writePwmFromCurrent();
};
