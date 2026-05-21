// StatusLed.cpp — state-machine driver for a single WS2812 NeoPixel.
//
// One write per state change. No cached "last colour" comparison, no
// redundant writes, no multi-backend type switching. The WS2812 holds
// its own state perfectly well; we just send a fresh frame whenever
// something changes.

#include "StatusLed.h"
#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <string.h>

// Pattern cadence for blink mode: 250 ms on, 250 ms off → 2 Hz.
static const uint32_t BLINK_HALF_PERIOD_MS = 250;

// NeoPixel brightness scaling (0..255). 32 keeps onboard pixels readable
// without dazzling at close range.
static const uint8_t  NEOPIXEL_BRIGHTNESS = 32;

enum Mode { MODE_SOLID, MODE_PULSE, MODE_BLINK, MODE_DARK };

static StatusLedConfig   s_cfg;
static bool              s_inited = false;
static Adafruit_NeoPixel s_pixel;

// State machine.
static Mode     s_mode      = MODE_SOLID;
static uint32_t s_modeStart = 0;
static uint32_t s_modeEnd   = 0;
static uint8_t  s_baseR  = 0, s_baseG  = 0, s_baseB  = 0;
static uint8_t  s_pulseR = 0, s_pulseG = 0, s_pulseB = 0;

static neoPixelType parseColorOrder(const char* s) {
    if (!strcasecmp(s, "RGB")) return NEO_RGB + NEO_KHZ800;
    if (!strcasecmp(s, "GRB")) return NEO_GRB + NEO_KHZ800;
    if (!strcasecmp(s, "BRG")) return NEO_BRG + NEO_KHZ800;
    if (!strcasecmp(s, "RBG")) return NEO_RBG + NEO_KHZ800;
    if (!strcasecmp(s, "GBR")) return NEO_GBR + NEO_KHZ800;
    if (!strcasecmp(s, "BGR")) return NEO_BGR + NEO_KHZ800;
    return NEO_GRB + NEO_KHZ800;
}

// Write a colour to the WS2812. Always sends a fresh frame.
static void write(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_inited) return;
    s_pixel.setPixelColor(0, s_pixel.Color(r, g, b));
    s_pixel.show();
}

namespace StatusLed {

void begin(const StatusLedConfig& cfg) {
    s_cfg = cfg;
    if (s_cfg.pin < 0) { s_inited = false; return; }

    s_pixel.updateLength(1);
    s_pixel.setPin(s_cfg.pin);
    s_pixel.updateType(parseColorOrder(s_cfg.colorOrder));
    s_pixel.begin(); // pin → OUTPUT, driven LOW

    // WS2812 needs >50 µs of sustained low to recognise a reset frame
    // separator. 300 µs is comfortably over spec.
    delayMicroseconds(300);

    s_pixel.setBrightness(NEOPIXEL_BRIGHTNESS);
    s_pixel.clear();
    s_pixel.show();

    s_inited = true;
}

void setSolid(uint8_t r, uint8_t g, uint8_t b) {
    s_baseR = r; s_baseG = g; s_baseB = b;
    s_mode = MODE_SOLID;
    write(r, g, b);
}

void beginPulse(uint8_t r, uint8_t g, uint8_t b, uint32_t ms) {
    s_pulseR = r; s_pulseG = g; s_pulseB = b;
    s_mode = MODE_PULSE;
    s_modeStart = millis();
    s_modeEnd   = s_modeStart + ms;
    write(r, g, b);
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
    write(0, 0, 0);
}

void off() { write(0, 0, 0); }

void update(uint32_t now) {
    if (s_mode == MODE_SOLID) return;

    if ((int32_t)(now - s_modeEnd) >= 0) {
        // Transient mode expired → back to solid base colour.
        s_mode = MODE_SOLID;
        write(s_baseR, s_baseG, s_baseB);
        return;
    }

    switch (s_mode) {
        case MODE_PULSE:
            // Pulse stays on the override colour until the timer expires.
            // The write below is redundant most ticks, but harmless and
            // keeps the code path uniform.
            write(s_pulseR, s_pulseG, s_pulseB);
            break;
        case MODE_BLINK: {
            uint32_t phase = ((now - s_modeStart) / BLINK_HALF_PERIOD_MS) % 2;
            if (phase == 0) write(s_baseR, s_baseG, s_baseB);
            else            write(0, 0, 0);
            break;
        }
        case MODE_DARK:
            write(0, 0, 0);
            break;
        default: break;
    }
}

} // namespace StatusLed
