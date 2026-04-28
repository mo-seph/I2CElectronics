// SERVO_MOTOR.h
//
// A standard RC-style servo (hobby servo) controlled by 50 Hz PWM.
// Parameter 0 (POSITION) maps to the servo angle between MIN_POSITION
// and MAX_POSITION config values (both in degrees, 0-180), translated
// to a pulse width in the range 500-2500 µs.
//
// Demo mode: continuous triangle-wave sweep from MIN to MAX and back,
// period ~5s, until stopDemo() is called.
//
// Implementation: uses arduino-esp32's LEDC API directly (ledcSetup /
// ledcAttachPin / ledcWrite). No ESP32Servo dependency — that library's
// pin validation and channel allocation logic was buggy on ESP32-S3.
// LEDC channels are allocated from 7 downward so they don't clash with
// analogWrite (used by PWM_MOTOR), which allocates upward from 0.
//
// Hardware notes:
//   - Servos typically want 5V and can draw significant current (peak
//     >500mA for small servos, more for larger). Don't try to power a
//     servo from the 3.3V rail or directly from USB-5V for anything but
//     the smallest servos — use an external 5V supply sharing GND with
//     the board.
//   - Signal is 3.3V PWM; most modern servos tolerate this on their
//     signal pin but older ones may need a level shifter.

#pragma once

#include "Element.h"
#include "Protocol.h"

class SERVO_MOTOR : public Element {
public:
    // Named parameter indices — use these instead of raw integers so
    // reads like `parameter(POSITION)` tell you what's being fetched.
    enum Param : uint8_t { POSITION = 0 };

    SERVO_MOTOR();
    virtual ~SERVO_MOTOR() override;

    bool configure(JsonObjectConst cfg) override;
    bool initialise() override;
    void allStop() override;

    uint8_t parameterCount() const override { return 1; }
    uint8_t typeId() const override { return AOProtocol::TYPE_SERVO_MOTOR; }
    const char* typeName() const override { return "SERVO_MOTOR"; }

protected:
    void updateControl(uint32_t dtMs) override;
    void updateDemo(uint32_t dtMs) override;
    void onDemoStart() override;

private:
    // Config values (from config.json).
    int pin = -1;
    int minPosition = 0;    // servo angle (deg) at POSITION = 0.0
    int maxPosition = 180;  // servo angle (deg) at POSITION = 1.0

    // Hardware state.
    int8_t   ledcChannel = -1;
    uint32_t pulseMinUs  = 0;   // precomputed in initialise()
    uint32_t pulseMaxUs  = 0;

    // Demo state: phase accumulator in ms, wraps on DEMO_PERIOD_MS.
    uint32_t demoPhaseMs = 0;

    // Last param we logged — so the per-write log fires only on change.
    uint16_t lastLoggedParam = 0xFFFF;

    // Map a param [0,65535] to pulse width and write to LEDC.
    void writeServoFromParam(uint16_t paramVal);
};
