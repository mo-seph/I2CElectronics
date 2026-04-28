// i2c-test/main.cpp
//
// Minimal self-contained I2C slave smoke-test for the Waveshare ESP32-S3-Zero.
// No LittleFS, no elements, no config — just Wire slave on a fixed
// address so we can prove (or disprove) that the hardware works.
//
// Flash with:  pio run -e peripheral-s3-test -t upload
// Monitor with: pio device monitor -e peripheral-s3-test
//
// On the Central, run:  scan_interval 2000
// If the slave is working you will see 0x13 appear in the scan list and
// the rx/tx counters below will advance.
//
// Default config (overridable via build flags):
//   I2C address : 0x13
//   SDA         : GPIO10
//   SCL         : GPIO11

#include <Arduino.h>
#include <Wire.h>

// Configurable via build flags (all named I2C_TEST_* to avoid clashing
// with the ESP32 SDK's own I2C_SLAVE_ADDR register definition):
//   -DI2C_TEST_SLAVE_ADDR=0x13  -DI2C_TEST_SDA_PIN=8  -DI2C_TEST_SCL_PIN=9
// Defaults: GPIO 8/9 — the ESP32-S3 variant defaults, known to work.
#ifndef I2C_TEST_SLAVE_ADDR
#  define I2C_TEST_SLAVE_ADDR  0x13
#endif
#ifndef I2C_TEST_SDA_PIN
#  define I2C_TEST_SDA_PIN 8
#endif
#ifndef I2C_TEST_SCL_PIN
#  define I2C_TEST_SCL_PIN 9
#endif

static const uint8_t SLAVE_ADDR = I2C_TEST_SLAVE_ADDR;
static const int     SDA_PIN    = I2C_TEST_SDA_PIN;
static const int     SCL_PIN    = I2C_TEST_SCL_PIN;

static volatile uint32_t rxCount  = 0;
static volatile uint32_t txCount  = 0;
static volatile uint8_t  lastByte = 0;

static void onReceive(int n) {
    rxCount++;
    while (Wire.available()) lastByte = (uint8_t)Wire.read();
}

static void onRequest() {
    txCount++;
    Wire.write((uint8_t)0xAA); // fixed sentinel — master can verify it
}

void setup() {
    Serial.begin(115200);
    delay(5000); // let the HWCDC terminal catch up

    Serial.println(F("# ========================"));
    Serial.println(F("# I2C slave smoke-test"));
    Serial.println(F("# ========================"));
    Serial.printf("# addr=0x%02X  SDA=GPIO%d  SCL=GPIO%d\n",
                  SLAVE_ADDR, SDA_PIN, SCL_PIN);

    Serial.printf("# free heap: %u  free PSRAM: %u\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getFreePsram());

    // Wire.setPins() stores the pin choice without touching GPIO hardware.
    // Wire.begin(addr) with no pin args then lets i2cSlaveInit() configure
    // the GPIO cleanly.  Passing pins directly to begin() causes a hang on
    // arduino-esp32 v2.x (IDF GPIO state conflicts with the slave driver).
    Wire.setPins(SDA_PIN, SCL_PIN);
    Wire.onReceive(onReceive);
    Wire.onRequest(onRequest);

    Serial.printf("# calling Wire.begin(0x%02X) SDA=GPIO%d SCL=GPIO%d...\n",
                  SLAVE_ADDR, SDA_PIN, SCL_PIN);
    Serial.flush();
    bool ok = Wire.begin(SLAVE_ADDR);
    Serial.printf("# Wire.begin() returned: %s\n", ok ? "true" : "false");

    if (ok) {
        Serial.println(F("# slave ready — waiting for master packets"));
        Serial.println(F("# rx and tx counters printed every 2 s when they change,"));
        Serial.println(F("# 'waiting' when idle."));
    } else {
        Serial.println(F("# Wire.begin() FAILED — slave not running"));
    }
}

void loop() {
    static uint32_t lastRx    = 0, lastTx = 0;
    static uint32_t lastPrint = 0;
    uint32_t now = millis();

    if (now - lastPrint >= 2000) {
        lastPrint = now;
        uint32_t rx = rxCount;
        uint32_t tx = txCount;
        if (rx != lastRx || tx != lastTx) {
            Serial.printf("# rx=%-4u tx=%-4u  last_byte=0x%02X\n",
                          (unsigned)rx, (unsigned)tx, (unsigned)lastByte);
            lastRx = rx;
            lastTx = tx;
        } else {
            Serial.println(F("# waiting..."));
        }
    }
}
