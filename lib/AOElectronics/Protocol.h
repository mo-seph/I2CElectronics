// Protocol.h
//
// Constants for the I2C protocol and parameter encoding. Mirrors
// shared/i2c_messages.json — keep them in sync by hand.
//
// Parameter encoding:
//   All parameters are uint16_t on the wire and internally. The semantic
//   range is [0, 65535] mapped to [0.0, 1.0]. Elements translate this to
//   hardware values using their own config.

#pragma once

#include <stdint.h>

namespace AOProtocol {

    // I2C message IDs (first byte of each message)
    static const uint8_t MSG_GET_CONFIG     = 0x30;
    static const uint8_t MSG_SET_PARAMETERS = 0x31;
    static const uint8_t MSG_STOP           = 0x32;
    static const uint8_t MSG_SET_PARAMETER  = 0x33; // single parameter
    static const uint8_t MSG_DEMO           = 0x34; // on/off per element or all

    // Text tunnel — carries a full USB Serial text command (+ response)
    // over I2C in chunks. Covers all non-realtime commands (cfg, board,
    // whoami, reboot, help, stop <idx>, etc.) without adding a binary
    // opcode per command. See shared/serial_protocol.md §Text tunnel.
    static const uint8_t MSG_TEXT_WRITE     = 0x40;
    static const uint8_t MSG_TEXT_READ      = 0x41;

    // Max payload bytes per TEXT_WRITE / TEXT_READ chunk. Chosen well
    // below the default Wire I2C buffer (128 on arduino-esp32 v2.x) to
    // leave room for headers on both sides.
    static const uint8_t TEXT_CHUNK_MAX     = 64;

    // Text-tunnel response `status` field values.
    static const uint8_t TEXT_STATUS_IN_PROGRESS = 0;
    static const uint8_t TEXT_STATUS_OK          = 1;
    static const uint8_t TEXT_STATUS_ERR         = 2;

    // Sentinel for "all elements on this peripheral" in MSG_DEMO.
    static const uint8_t DEMO_ALL_ELEMENTS  = 0xFF;

    // Parameter encoding
    static const uint8_t PARAM_BYTES = 2; // uint16_t little-endian
    static const uint16_t PARAM_MIN  = 0;
    static const uint16_t PARAM_MAX  = 65535;
    static const uint16_t PARAM_MID  = 32768; // approx "rest" / 0.5

    // Element-type IDs (must match control_schema.json)
    static const uint8_t TYPE_PWM_MOTOR       = 1;
    static const uint8_t TYPE_SERVO_MOTOR     = 2;
    static const uint8_t TYPE_SERVO_ARM       = 3;
    static const uint8_t TYPE_CONTINUOUS_SERVO = 4;

    // Board limits
    static const uint8_t MAX_ELEMENTS_PER_BOARD = 16;

    // Convert a uint16 parameter in [0,65535] to a float in [0.0,1.0].
    // Useful when mapping to hardware values.
    inline float paramToFloat(uint16_t v) {
        return (float)v / 65535.0f;
    }

    // Convert a float in [0.0,1.0] to a uint16 parameter.
    // Values outside the range are clamped.
    inline uint16_t floatToParam(float f) {
        if (f <= 0.0f) return 0;
        if (f >= 1.0f) return 65535;
        return (uint16_t)(f * 65535.0f + 0.5f);
    }

} // namespace AOProtocol
