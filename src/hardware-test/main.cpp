// hardware-test/main.cpp
//
// Minimal hardware-verification target.  Two jobs:
//
//   1. Cycle the on-board NeoPixel through visually-distinct patterns
//      per colour channel so you can confirm both LED operation AND the
//      configured colour order:
//
//        GREEN — fades smoothly from off to full over 3 s
//        RED   — blinks crisply at 2 Hz for 3 s
//        BLUE  — brief 100 ms pulses every 1 s for 3 s
//        WHITE — all channels at full for 2 s
//
//      Each phase logs what it's about to do on Serial, so if you see
//      "GREEN fade" on Serial but red on the LED, the colour_order
//      defined below doesn't match the hardware. Off-time separators
//      between phases keep transitions unambiguous.
//
//   2. Run an I2C slave at a fixed address so a Central can probe for
//      liveness with `scan` / `list`.
//
// Deliberately NOT using AOElectronics/StatusLed or ConfigLoader — this
// target talks to Adafruit_NeoPixel directly, with no LittleFS and no
// state machine.  That way if this test misbehaves, the suspect set is
// just WS2812 wiring + Adafruit_NeoPixel + ESP32-S3 timing, with none
// of our own indicator-layer code in the picture.
//
// Flash with:  pio run -e hardware-test -t upload
// Monitor:     pio device monitor -e hardware-test
//
// Build-flag overridables:
//   -DLED_TEST_PIN=<gpio>          GPIO driving the data line (default 9)
//   -DLED_TEST_COUNT=<n>           Number of pixels in the chain (default 1)
//   -DI2C_TEST_SLAVE_ADDR=<addr>   I2C slave address (default 0x14)
//   -DI2C_TEST_SDA_PIN=<gpio>      (default 5 — XIAO ESP32-S3 D4)
//   -DI2C_TEST_SCL_PIN=<gpio>      (default 6 — XIAO ESP32-S3 D5)
//
// Colour order is NOT a build flag.  If you see the wrong channel
// fading/blinking/pulsing, change NEO_GRB on the constructor line below
// to NEO_RGB / NEO_BRG / NEO_RBG / NEO_GBR / NEO_BGR until the labels
// match the lights — that gives you the right `color_order` value for
// board.json.

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_NeoPixel.h>

#ifndef LED_TEST_PIN
#  define LED_TEST_PIN 9
#endif
#ifndef LED_TEST_COUNT
#  define LED_TEST_COUNT 1
#endif
#ifndef I2C_TEST_SLAVE_ADDR
#  define I2C_TEST_SLAVE_ADDR  0x14
#endif
#ifndef I2C_TEST_SDA_PIN
#  define I2C_TEST_SDA_PIN 5
#endif
#ifndef I2C_TEST_SCL_PIN
#  define I2C_TEST_SCL_PIN 6
#endif

// Change NEO_GRB → NEO_RGB / NEO_BRG / etc. here if the labels in the
// Serial log don't match the visible LED colour.
static Adafruit_NeoPixel pixel(LED_TEST_COUNT, LED_TEST_PIN, NEO_GRB + NEO_KHZ800);

// ----- LED cycle timeline (ms) ----------------------------------------
// Phase boundaries kept as #defines so it's easy to skim the timeline.
#define T_GREEN_END     3000UL
#define T_GAP1_END      3500UL
#define T_RED_END       6500UL
#define T_GAP2_END      7000UL
#define T_BLUE_END     10000UL
#define T_GAP3_END     10500UL
#define T_WHITE_END    12500UL
#define T_CYCLE_END    13000UL

static const uint8_t LED_BRIGHTNESS = 32; // overall scale (matches StatusLed)

// ----- I2C slave -------------------------------------------------------
static volatile uint32_t i2cRxCount = 0;
static volatile uint32_t i2cTxCount = 0;

static void onReceive(int /*n*/) {
    i2cRxCount++;
    while (Wire.available()) (void)Wire.read();
}

static void onRequest() {
    i2cTxCount++;
    // Sentinel byte different from i2c-test's 0xAA, so a Central can
    // tell which test target it's talking to.
    Wire.write((uint8_t)0xBB);
}

// ----- LED rendering ---------------------------------------------------

// Write one colour to every pixel in the chain.
static void fill(uint8_t r, uint8_t g, uint8_t b) {
    for (uint16_t i = 0; i < LED_TEST_COUNT; i++) {
        pixel.setPixelColor(i, pixel.Color(r, g, b));
    }
    pixel.show();
}

// Compute colour for this point in the cycle.  Returns true and sets
// *phaseOut / *labelOut whenever a phase boundary is crossed since the
// last call, so the caller can log transitions.
static bool computeColour(uint32_t now, uint8_t* r, uint8_t* g, uint8_t* b,
                          int* phaseOut, const char** labelOut) {
    static int lastPhase = -1;
    uint32_t t = now % T_CYCLE_END;

    uint8_t  rr = 0, gg = 0, bb = 0;
    int      phase;
    const char* label;

    if      (t < T_GREEN_END)  { phase = 0; label = "GREEN — smooth fade up";
                                  gg = (uint8_t)((255UL * t) / T_GREEN_END); }
    else if (t < T_GAP1_END)   { phase = 1; label = "off"; }
    else if (t < T_RED_END)    { phase = 2; label = "RED — blink @ 2 Hz";
                                  uint32_t bp = (t - T_GAP1_END) / 250;
                                  if ((bp & 1) == 0) rr = 255; }
    else if (t < T_GAP2_END)   { phase = 3; label = "off"; }
    else if (t < T_BLUE_END)   { phase = 4; label = "BLUE — pulse 100 ms / 1 s";
                                  uint32_t pp = (t - T_GAP2_END) % 1000;
                                  if (pp < 100) bb = 255; }
    else if (t < T_GAP3_END)   { phase = 5; label = "off"; }
    else if (t < T_WHITE_END)  { phase = 6; label = "WHITE — all channels full";
                                  rr = gg = bb = 255; }
    else                       { phase = 7; label = "off (cycle restarting)"; }

    *r = rr; *g = gg; *b = bb;
    bool changed = (phase != lastPhase);
    if (changed) { *phaseOut = phase; *labelOut = label; lastPhase = phase; }
    return changed;
}

// ----- Arduino entry points -------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println(F("# =========================="));
    Serial.println(F("# AO ELECTRONICS — HARDWARE TEST"));
    Serial.println(F("# =========================="));
    Serial.printf("# LED: pin=GPIO%d count=%u (change NEO_GRB → NEO_xxx in source if colours mismatch)\n",
                  (int)LED_TEST_PIN, (unsigned)LED_TEST_COUNT);
    Serial.printf("# I2C: addr=0x%02X SDA=GPIO%d SCL=GPIO%d sentinel=0xBB\n",
                  (unsigned)I2C_TEST_SLAVE_ADDR,
                  (int)I2C_TEST_SDA_PIN, (int)I2C_TEST_SCL_PIN);

    // LED bring-up.
    pixel.begin();
    delayMicroseconds(300); // WS2812 reset-pulse window
    pixel.setBrightness(LED_BRIGHTNESS);
    pixel.clear();
    pixel.show();

    // I2C slave — same pattern as i2c-test smoke test.  Wire.setPins
    // before Wire.begin(addr) (NOT Wire.begin(addr, sda, scl) directly,
    // which hangs the slave driver on arduino-esp32 v2.x).
    Wire.setPins(I2C_TEST_SDA_PIN, I2C_TEST_SCL_PIN);
    Wire.onReceive(onReceive);
    Wire.onRequest(onRequest);
    bool ok = Wire.begin((uint8_t)I2C_TEST_SLAVE_ADDR);
    Serial.printf("# Wire.begin() returned %s\n", ok ? "true" : "false");

    Serial.println(F("# starting LED cycle — watch the LED and the phase labels below"));
}

void loop() {
    uint32_t now = millis();

    uint8_t r, g, b;
    int phase;
    const char* label;
    bool phaseChanged = computeColour(now, &r, &g, &b, &phase, &label);
    if (phaseChanged) {
        Serial.printf("# t=%lus  phase %d: %s\n",
                      (unsigned long)(now / 1000), phase, label);
    }
    fill(r, g, b);

    // Periodic I2C activity report so we can confirm the slave is alive
    // independent of the LED.
    static uint32_t lastI2cLog = 0;
    if (now - lastI2cLog >= 5000) {
        lastI2cLog = now;
        Serial.printf("# i2c rx=%u tx=%u\n",
                      (unsigned)i2cRxCount, (unsigned)i2cTxCount);
    }

    delay(20); // ~50 Hz update rate — smooth fading without flooding RMT
}
