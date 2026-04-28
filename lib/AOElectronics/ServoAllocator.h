// ServoAllocator.h
//
// Shared LEDC channel allocator for all Elements that drive RC-style
// servos directly via LEDC (SERVO_MOTOR, SERVO_ARM, eventually
// CONTINUOUS_SERVO). Allocates from channel 7 downward so it doesn't
// collide with analogWrite() — which PWM_MOTOR uses — which allocates
// from channel 0 upward.
//
// The ESP32-S3 has 8 LEDC channels total, so a board can mix up to 8
// PWM- or servo-driven Elements before the pool is exhausted.

#pragma once

namespace ServoAllocator {

// Allocate the next free LEDC channel for servo PWM. Returns -1 if the
// pool is exhausted. Not thread-safe — only call from setup/init paths.
int allocate();

} // namespace ServoAllocator
