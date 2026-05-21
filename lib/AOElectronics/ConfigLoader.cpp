// ConfigLoader.cpp
//
// Parses /config.json and /board.json from LittleFS. Elements are created
// and configure()d here, but NOT initialise()d — the caller does hardware
// bringup as a separate, inspectable step.

#include "ConfigLoader.h"
#include "ElementFactory.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

static const char* BOARD_PATH   = "/board.json";
static const char* CONFIG_PATH  = "/config.json";

bool ConfigLoader::mount() {
    if (!LittleFS.begin(true)) { // true = format on failure
        Serial.println(F("# [ConfigLoader] LittleFS mount failed"));
        return false;
    }
    return true;
}

bool ConfigLoader::loadBoardConfig(BoardConfig& out) {
    File f = LittleFS.open(BOARD_PATH, "r");
    if (!f) {
        Serial.println(F("# [ConfigLoader] board.json missing; using defaults"));
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.printf("# [ConfigLoader] board.json parse error: %s\n", err.c_str());
        return false;
    }

    if (doc["i2c_address"].is<int>()) {
        out.i2cAddress = (uint8_t)doc["i2c_address"].as<int>();
    }

    // status_led block — optional. Missing or pin<0 → no LED.
    JsonVariantConst led = doc["status_led"];
    if (led.is<JsonObjectConst>()) {
        out.statusLed.pin = led["pin"] | -1;
        const char* order = led["color_order"] | "GRB";
        size_t cap = sizeof(out.statusLed.colorOrder);
        strncpy(out.statusLed.colorOrder, order, cap - 1);
        out.statusLed.colorOrder[cap - 1] = 0;
    }

    return true;
}

bool ConfigLoader::loadElements(Element** elements,
                                uint8_t maxElements,
                                uint8_t& outCount) {
    outCount = 0;

    File f = LittleFS.open(CONFIG_PATH, "r");
    if (!f) {
        Serial.println(F("# [ConfigLoader] config.json missing"));
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.printf("# [ConfigLoader] config.json parse error: %s\n", err.c_str());
        return false;
    }

    JsonArrayConst arr = doc.as<JsonArrayConst>();
    if (arr.isNull()) {
        Serial.println(F("# [ConfigLoader] config.json: top-level must be an array"));
        return false;
    }

    for (JsonVariantConst entry : arr) {
        if (outCount >= maxElements) {
            Serial.printf("# [ConfigLoader] config has more than %u elements; ignoring rest\n",
                          maxElements);
            break;
        }

        const char* typeName = entry["type"] | (const char*)nullptr;
        if (typeName == nullptr) {
            Serial.println(F("# [ConfigLoader] skipping entry: missing 'type'"));
            continue;
        }

        Element* el = ElementFactory::createElement(typeName);
        if (el == nullptr) {
            continue; // factory already logged
        }

        JsonObjectConst cfg = entry["config"].as<JsonObjectConst>();
        if (!el->configure(cfg)) {
            Serial.printf("# [ConfigLoader] configure failed for %s, skipping\n", typeName);
            delete el;
            continue;
        }

        elements[outCount++] = el;
        Serial.printf("# [ConfigLoader] configured Element %u: %s\n",
                      outCount, typeName);
    }

    return true;
}
