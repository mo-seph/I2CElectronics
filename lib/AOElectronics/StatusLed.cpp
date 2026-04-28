// StatusLed.cpp
//
// Two hardware backends, selected at build time:
//
//   Default (no flag)     — Adafruit NeoPixel addressable RGB LED.
//                           -DSTATUS_LED_NEO_GRB selects GRB colour order
//                           (external WS2812Bs); default is NEO_RGB
//                           (Waveshare ESP32-S3-Zero onboard pixel).
//
//   -DSTATUS_LED_SIMPLE   — plain on/off GPIO LED (e.g. XIAO ESP32-C6
//                           GPIO15 "User Light").  All colour calls treat
//                           any non-zero channel as "on".  Add
//                           -DSTATUS_LED_ACTIVE_LOW if the LED sinks
//                           to GND through the GPIO (LOW = on).
//
// Pin:  -DSTATUS_LED_GPIO=<n>  (default 21 — Waveshare S3-Zero WS2812)

#include "StatusLed.h"
#include <Arduino.h>

#ifndef STATUS_LED_GPIO
#  define STATUS_LED_GPIO 21
#endif
static const int STATUS_LED_PIN = STATUS_LED_GPIO;

// ---- NeoPixel backend (default) ----
#ifndef STATUS_LED_SIMPLE
#  include <Adafruit_NeoPixel.h>
static const uint8_t DEFAULT_BRIGHT = 32;
#  ifdef STATUS_LED_NEO_GRB
static Adafruit_NeoPixel pixel(1, STATUS_LED_PIN, NEO_GRB + NEO_KHZ800);
#  else
static Adafruit_NeoPixel pixel(1, STATUS_LED_PIN, NEO_RGB + NEO_KHZ800);
#  endif
// Last colour written — skip redundant show() calls.
static int16_t s_lastR = -1, s_lastG = -1, s_lastB = -1;
#endif // !STATUS_LED_SIMPLE

// Pattern cadence for blink mode: 250 ms on, 250 ms off → 2 Hz.
static const uint32_t BLINK_HALF_PERIOD_MS = 250;

enum Mode { MODE_SOLID, MODE_PULSE, MODE_BLINK, MODE_DARK };

static Mode     s_mode      = MODE_SOLID;
static uint32_t s_modeStart = 0;
static uint32_t s_modeEnd   = 0;

// Base (solid) role colour — returned to when a transient mode expires.
static uint8_t s_baseR = 0, s_baseG = 0, s_baseB = 0;
// Pulse override colour.
static uint8_t s_pulseR = 0, s_pulseG = 0, s_pulseB = 0;

// Write a colour to the hardware.  Called by all state transitions.
static void physicalSet(uint8_t r, uint8_t g, uint8_t b) {
#ifdef STATUS_LED_SIMPLE
    // Simple LED: any non-zero channel → on.
    bool on = (r || g || b);
#  ifdef STATUS_LED_ACTIVE_LOW
    digitalWrite(STATUS_LED_PIN, on ? LOW : HIGH);
#  else
    digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
#  endif
#else
    // NeoPixel: skip write when colour is unchanged.
    if (r == (uint8_t)s_lastR &&
        g == (uint8_t)s_lastG &&
        b == (uint8_t)s_lastB) return;
    pixel.setPixelColor(0, pixel.Color(r, g, b));
    pixel.show();
    s_lastR = r; s_lastG = g; s_lastB = b;
#endif
}

namespace StatusLed {

void begin() {
#ifdef STATUS_LED_SIMPLE
    pinMode(STATUS_LED_PIN, OUTPUT);
    physicalSet(0, 0, 0); // off
#else
    pixel.begin();
    pixel.setBrightness(DEFAULT_BRIGHT);
    pixel.clear();
    pixel.show();
    s_lastR = s_lastG = s_lastB = -1; // force first write
#endif
}

void setSolid(uint8_t r, uint8_t g, uint8_t b) {
    s_baseR = r; s_baseG = g; s_baseB = b;
    s_mode = MODE_SOLID;
    physicalSet(r, g, b);
}

void beginPulse(uint8_t r, uint8_t g, uint8_t b, uint32_t ms) {
    s_pulseR = r; s_pulseG = g; s_pulseB = b;
    s_mode = MODE_PULSE;
    s_modeStart = millis();
    s_modeEnd   = s_modeStart + ms;
    physicalSet(r, g, b);
}

void beginBlink(uint32_t ms) {
    s_mode = MODE_BLINK;
    s_modeStart = millis();
    s_modeEnd   = s_modeStart + ms;
}

void beginDark(uint32_t ms) {
    s_mode = MODE_DARK;
    s_modeStart = millis();
    s_modeEnd   = s_modeStart + ms;
    physicalSet(0, 0, 0);
}

void off() { physicalSet(0, 0, 0); }

void update(uint32_t now) {
    if (s_mode == MODE_SOLID) return;

    if ((int32_t)(now - s_modeEnd) >= 0) {
        // Transient mode expired → back to solid role colour.
        s_mode = MODE_SOLID;
        physicalSet(s_baseR, s_baseG, s_baseB);
        return;
    }

    switch (s_mode) {
        case MODE_PULSE:
            physicalSet(s_pulseR, s_pulseG, s_pulseB);
            break;
        case MODE_BLINK: {
            uint32_t phase = ((now - s_modeStart) / BLINK_HALF_PERIOD_MS) % 2;
            if (phase == 0) physicalSet(s_baseR, s_baseG, s_baseB);
            else            physicalSet(0, 0, 0);
            break;
        }
        case MODE_DARK:
            physicalSet(0, 0, 0);
            break;
        default: break;
    }
}

} // namespace StatusLed
