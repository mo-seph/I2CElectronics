// BLDC_MOTOR.h
//
// A brushless motor driven through a BLHeli (or other PWM/PPM-compatible)
// ESC. The ESC sees the standard 1-2 ms hobby-servo style pulse train:
//
//   ~1000 µs pulse  →  zero throttle (armed but stopped)
//   ~2000 µs pulse  →  full throttle
//   pulse rate:        50 Hz default; BLHeli_S accepts up to ~500 Hz
//
// Arming behaviour: at boot the ESC needs to see a sustained
// MIN_PULSE_US for ARM_DURATION_MS before it will accept throttle commands.
// This is handled inside initialise() — the LEDC duty is set to the
// MIN_PULSE_US equivalent and the call blocks via delay() for the arming
// window. Multiple BLDC_MOTORs therefore arm sequentially during boot
// (2s × N for N motors). See WORKING.md for the planned parallelisation.
//
// SAFETY: brushless motors with no propeller/wheel attached can spin up
// to dangerous speeds at modest throttle values. The MAX_PULSE_US config
// field is your safety cap — leave it at 1100-1300 µs while bench testing.
//
// Parameter 0 (THROTTLE) maps linearly to the [MIN_PULSE_US..MAX_PULSE_US]
// pulse range. RAMP_TIME limits how fast the commanded throttle can
// change in software, independent of the ESC's own ramping.
//
// Implementation: direct LEDC, 14-bit resolution (the cap imposed by
// arduino-esp32 v2.x's LEDC layer — same as SERVO_MOTOR uses). At 50 Hz
// that's ~1.22 µs per LSB, ~820 LSBs across the 1000-2000 µs throttle
// range — plenty for any ESC. Channel allocated from ServoAllocator
// (counts down from 7) so it shares the LEDC pool with SERVO_MOTOR /
// SERVO_ARM and stays clear of PWM_MOTOR (which counts up from 0).

#pragma once

#include "Element.h"
#include "Protocol.h"

class BLDC_MOTOR : public Element {
public:
    enum Param : uint8_t { THROTTLE = 0 };

    BLDC_MOTOR();
    virtual ~BLDC_MOTOR() override;

    bool configure(JsonObjectConst cfg) override;
    bool initialise() override;
    void allStop() override;

    uint8_t parameterCount() const override { return 1; }
    uint8_t typeId() const override { return AOProtocol::TYPE_BLDC_MOTOR; }
    const char* typeName() const override { return "BLDC_MOTOR"; }

protected:
    void updateControl(uint32_t dtMs) override;
    void updateDemo(uint32_t dtMs) override;
    void onDemoStart() override;

private:
    // Config (from config.json — set in configure(), used in initialise()).
    int      pin            = -1;
    uint32_t minPulseUs     = 1000;
    uint32_t maxPulseUs     = 2000;
    uint32_t updateFreqHz   = 50;
    uint32_t armDurationMs  = 2000;
    uint32_t rampTimeMs     = 200;

    // Hardware state.
    int8_t   ledcChannel    = -1;
    // Period (µs) for the chosen update frequency. Precomputed in init.
    uint32_t periodUs       = 20000;
    // Maximum LEDC duty value for the chosen resolution.
    uint32_t dutyMax        = 0;

    // Software ramp state — currentParam slews toward parameter(THROTTLE)
    // at most 65535*dtMs/rampTimeMs per tick. Identical pattern to PWM_MOTOR.
    uint16_t currentParam   = 0;

    // Demo state: phase accumulator in ms, wraps on DEMO_PERIOD_MS.
    uint32_t demoPhaseMs    = 0;

    // Last param we logged — change-only logging to avoid spam.
    uint16_t lastLoggedParam = 0xFFFF;

    // Map currentParam [0..65535] → pulse width → LEDC duty → ledcWrite.
    void writeFromCurrent();
    // Ramp currentParam toward parameter(THROTTLE) and write.
    void rampAndWrite(uint32_t dtMs);
};
