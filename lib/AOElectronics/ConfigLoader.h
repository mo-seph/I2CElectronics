// ConfigLoader.h
//
// Loads a Peripheral's configuration from LittleFS:
//   - /config.json : list of Elements and their config values (see
//                    shared/example_elements.json for the schema).
//   - /board.json  : board-specific settings, currently just "i2c_address".
//
// The Peripheral calls ElementFactory for each entry in config.json and
// stores the resulting Element*s in an array owned by the caller.

#pragma once

#include "Element.h"
#include "Protocol.h"

struct BoardConfig {
    uint8_t i2cAddress = 0x10; // default if board.json is missing/invalid
};

class ConfigLoader {
public:
    // Mount LittleFS. Returns false if mount fails.
    static bool mount();

    // Load /board.json into `out`. Returns false if file missing or invalid.
    // Leaves `out` at defaults on failure.
    static bool loadBoardConfig(BoardConfig& out);

    // Load /config.json and populate elements[] with newly-allocated Elements.
    // Writes the final count into outCount. Caller owns the pointers.
    // Returns false on fatal error (e.g. file missing, malformed JSON).
    // Individual element failures are logged but do not abort the whole load.
    static bool loadElements(Element** elements,
                             uint8_t maxElements,
                             uint8_t& outCount);
};
