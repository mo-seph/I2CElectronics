// StatusLed.h
//
// A small state machine driving a single WS2812 (NeoPixel) status LED.
//
//   Solid         — base role colour (green = Peripheral, cyan = Central)
//   Pulse(c, ms)  — briefly show an override colour, then revert to Solid
//                   (used for "cfg saved" confirmation flashes)
//   Blink(ms)     — flash the base colour on/off for a duration, then
//                   revert to Solid (used by the `blink` identify command)
//   Dark(ms)      — LED off for a duration, then revert to Solid (used by
//                   `ledoff` to spot crashed boards against a sea of dark)
//
// `update(now)` must be called every loop tick to advance transient modes
// back to Solid when their timer expires.
//
// Configured at runtime from /board.json (`status_led` block: `pin` and
// `color_order`).  Omit the block, or set `pin` to a negative value, to
// run without a status LED.

#pragma once

#include <stdint.h>

struct StatusLedConfig {
    int  pin = -1;              // GPIO; negative disables the LED entirely
    char colorOrder[8] = "GRB"; // any 3-letter R/G/B permutation
};

namespace StatusLed {

// Apply the config and bring the hardware up. Safe to call once after
// board.json has been parsed.
void begin(const StatusLedConfig& cfg);

// Set the base role colour (mode = Solid) and write it.
void setSolid(uint8_t r, uint8_t g, uint8_t b);

// Show a single colour for `ms` milliseconds, then return to Solid.
void beginPulse(uint8_t r, uint8_t g, uint8_t b, uint32_t ms);

// Flash the base colour on and off for `ms` ms, then return to Solid.
void beginBlink(uint32_t ms);

// Turn the LED off for `ms` ms, then return to Solid.
void beginDark(uint32_t ms);

// Off right now, no mode change.
void off();

// Advance the state machine. Call from loop() each iteration.
void update(uint32_t nowMs);

} // namespace StatusLed
