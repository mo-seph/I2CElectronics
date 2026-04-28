// StatusLed.h
//
// The single onboard WS2812B (GPIO 21 on the Waveshare ESP32-S3-Zero)
// drives a tiny state machine so it can signal several things:
//
//   Solid         — base role colour (green = Peripheral, cyan = Central)
//   Pulse(c, ms)  — briefly show an override colour, then revert
//                   (e.g. white flash on successful cfg/board save)
//   Blink(ms)     — flash the base colour on/off for a duration
//                   (used by the `blink` command to identify a device)
//   Dark(ms)      — LED off for a duration (used by `ledoff` to spot
//                   crashed/stuck boards against a sea of dark ones)
//
// `update(now)` must be called every loop tick — it advances transient
// modes back to Solid when their timer expires. LED hardware writes are
// throttled: we only call `pixel.show()` when the displayed colour
// actually changes.

#pragma once

#include <stdint.h>

namespace StatusLed {

// Call once in setup() before any of the other functions.
void begin();

// Set the base role colour and show it immediately (mode = Solid).
void setSolid(uint8_t r, uint8_t g, uint8_t b);

// Show a single colour for `ms` milliseconds, then return to Solid
// with the previous base colour. Good for transient feedback (cfg
// save, errors, etc.).
void beginPulse(uint8_t r, uint8_t g, uint8_t b, uint32_t ms);

// Flash the base colour on and off for `ms` milliseconds, then return
// to Solid. Used by the `blink` identify command.
void beginBlink(uint32_t ms);

// Turn the LED off for `ms` milliseconds, then return to Solid. Used
// by the `ledoff` census command.
void beginDark(uint32_t ms);

// Convenience: set LED off right now, leave Solid base colour unchanged.
// (For clean shutdown states.)
void off();

// Advance the state machine. Call from loop() each iteration.
void update(uint32_t nowMs);

} // namespace StatusLed
