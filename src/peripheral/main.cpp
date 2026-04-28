// Peripheral main.cpp
//
// Boots the Peripheral, loads config from LittleFS, initialises Elements,
// and runs an update loop. A line-based USB Serial CLI handles both human
// interaction and GUI/script access.
//
// Protocol: see shared/serial_protocol.md.
//   - Commands end with \n.
//   - Every command produces exactly one terminator line: `OK` on success,
//     `ERR <reason>` on failure. Multi-line data appears before the
//     terminator.
//   - Log lines always begin with `# ` so hosts can filter them out.
//
// CLI commands (see serial_protocol.md for authoritative spec):
//   whoami                    - identify device role + id
//   list                      - list loaded Elements
//   cfg                       - dump config.json
//   cfg <json>                - replace config.json (compact JSON on one line)
//   board                     - dump board.json
//   board <json>              - replace board.json
//   reboot                    - restart (no OK — connection drops)
//   stop | s                  - stop all Elements
//   stop <idx>                - stop one Element
//   p <idx> <f1> [f2 ...]     - set all parameters (floats in [0.0, 1.0])
//   pi <idx> <pid> <f>        - set one parameter (pid is 0-based index)
//   demo <idx|all> on|off     - continuous demo on/off
//   help | ?                  - show help

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Wire.h>

#include "Element.h"
#include "ConfigLoader.h"
#include "Protocol.h"
#include "StatusLed.h"

using namespace AOProtocol;

// ------------------------- Tuning -------------------------

// Main loop cadence.
static uint32_t LOOP_DELAY_MS = 50;

// ------------------------- State -------------------------

static Element* elements[MAX_ELEMENTS_PER_BOARD];
static uint8_t elementCount = 0;
static BoardConfig board;
static uint32_t lastUpdateMs = 0;

// ------------------------- CLI state -------------------------

// Up to 4 KB so cfg <json> payloads for a fully-populated board fit on
// one line.
static const uint16_t CLI_BUF_SIZE = 4096;
static char cliBuf[CLI_BUF_SIZE];
static uint16_t cliLen = 0;

// Cleared on the first command received from any source. Drives a
// startup heartbeat in loop() so serial connectivity can be confirmed
// even when boot messages are missed due to USB-CDC open-port timing.
static bool aliveAcked = false;

// ------------------------- Command output redirection -------------------------
//
// Command handlers write their response via *g_out. Normally this is
// &Serial (USB Serial CLI). When a text-tunnel request is being
// processed, pumpI2C() swaps g_out to a BufferPrint so the response is
// captured into textRespBuf for the Central to read back in chunks.
//
// ok() / err() also flip flags so the tunnel can tell whether the
// command completed with OK or ERR — this drives the response STATUS
// byte reported via TEXT_READ.

class BufferPrint : public Print {
public:
    BufferPrint(char* buf, size_t cap) : _buf(buf), _cap(cap), _pos(0) {}
    size_t write(uint8_t c) override {
        if (_pos < _cap) _buf[_pos++] = (char)c;
        return 1;
    }
    size_t write(const uint8_t* b, size_t n) override {
        size_t room = (_pos < _cap) ? (_cap - _pos) : 0;
        size_t copy = (n < room) ? n : room;
        for (size_t i = 0; i < copy; i++) _buf[_pos + i] = (char)b[i];
        _pos += copy;
        return n; // report full for caller — excess bytes silently dropped
    }
    void   reset()          { _pos = 0; }
    size_t size() const     { return _pos; }
private:
    char* _buf;
    size_t _cap;
    size_t _pos;
};

static Print* g_out         = &Serial;
static bool   cmdEmittedOk  = false;
static bool   cmdEmittedErr = false;

// ------------------------- helpers -------------------------

static void ok()                 { g_out->println(F("OK")); cmdEmittedOk  = true; }
static void err(const char* msg) { g_out->printf("ERR %s\n", msg); cmdEmittedErr = true; }

static void printHelp() {
    g_out->println(F(
        "Commands:\n"
        "  whoami                    - device role + id\n"
        "  list                      - list loaded Elements\n"
        "  cfg                       - dump config.json\n"
        "  cfg <json>                - replace config.json\n"
        "  board                     - dump board.json\n"
        "  board <json>              - replace board.json\n"
        "  reboot                    - restart the device\n"
        "  stop | s [idx]            - stop all Elements (or one)\n"
        "  p <idx> <f1> [f2..]       - set all parameters (floats in [0,1])\n"
        "  pi <idx> <pid> <f>        - set one parameter (pid is 0-based)\n"
        "  demo <idx|all> on|off     - continuous demo on/off\n"
        "  blink [seconds]           - flash status LED to identify this board\n"
        "  ledoff [seconds]          - turn status LED off (spot crashed boards)\n"
        "  help | ?                  - show this help"
    ));
}

// Dump file contents to Serial. Reads in 64-byte chunks. Ensures output
// ends with \n so the caller can emit the terminator on its own line.
static bool dumpFile(const char* path) {
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    uint8_t buf[64];
    int lastByte = 0;
    while (f.available()) {
        size_t n = f.read(buf, sizeof(buf));
        g_out->write(buf, n);
        if (n > 0) lastByte = buf[n - 1];
    }
    f.close();
    if (lastByte != '\n') g_out->println();
    return true;
}

// Return the 1-based-index Element, or nullptr if out of range.
static Element* elementByIndex(int idx) {
    if (idx < 1 || idx > (int)elementCount) return nullptr;
    return elements[idx - 1];
}

// Write `payload` (bounded by `len`) atomically over the given path. Uses
// a `.new` temp file + rename so a failed write doesn't leave a corrupt
// file behind.
static bool writeFileAtomic(const char* path, const char* payload, size_t len) {
    String tmpPath = String(path) + ".new";
    File f = LittleFS.open(tmpPath.c_str(), "w");
    if (!f) return false;
    size_t written = f.write((const uint8_t*)payload, len);
    f.close();
    if (written != len) {
        LittleFS.remove(tmpPath.c_str());
        return false;
    }
    // Remove existing then rename. LittleFS rename does not overwrite.
    LittleFS.remove(path);
    if (!LittleFS.rename(tmpPath.c_str(), path)) {
        LittleFS.remove(tmpPath.c_str());
        return false;
    }
    return true;
}

// Validate that `payload` is syntactically valid JSON. We don't deep-check
// schema here — ConfigLoader at next boot will reject bad structure.
static bool isValidJson(const char* payload, size_t len) {
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, payload, len);
    return !e;
}

// ------------------------- commands -------------------------

static void cmdWhoami() {
    g_out->printf("WHOAMI role=peripheral id=%u\n", board.i2cAddress);
    ok();
}

static void cmdList() {
    g_out->printf("%u Element(s) loaded:\n", elementCount);
    for (uint8_t i = 0; i < elementCount; i++) {
        g_out->printf("  %u: %s (type_id=%u, params=%u, demo=%s)\n",
                      i + 1,
                      elements[i]->typeName(),
                      elements[i]->typeId(),
                      elements[i]->parameterCount(),
                      elements[i]->isDemoActive() ? "on" : "off");
    }
    ok();
}

// `stop` (all) or `stop <idx>` (single element).
static void cmdStop(const char* args) {
    if (args && *args) {
        int idx = atoi(args);
        Element* el = elementByIndex(idx);
        if (!el) { err("no element at that index"); return; }
        el->allStop();
        g_out->printf("stopped %d\n", idx);
    } else {
        for (uint8_t i = 0; i < elementCount; i++) elements[i]->allStop();
        g_out->println(F("stopped"));
    }
    ok();
}

// `cfg` or `cfg <json>` — args empty = read, else write.
static void cmdCfg(const char* args) {
    if (!args || !*args) {
        if (!dumpFile("/config.json")) { err("file not found"); return; }
        ok(); return;
    }
    size_t n = strlen(args);
    if (!isValidJson(args, n))             { err("invalid json"); return; }
    if (!writeFileAtomic("/config.json", args, n)) { err("write failed"); return; }
    // Visible confirmation — brief white flash, then back to role green.
    StatusLed::beginPulse(255, 255, 255, 300);
    ok();
}

// `board` or `board <json>`.
static void cmdBoard(const char* args) {
    if (!args || !*args) {
        if (!dumpFile("/board.json")) { err("file not found"); return; }
        ok(); return;
    }
    size_t n = strlen(args);
    if (!isValidJson(args, n))            { err("invalid json"); return; }
    if (!writeFileAtomic("/board.json", args, n)) { err("write failed"); return; }
    StatusLed::beginPulse(255, 255, 255, 300);
    ok();
}

// `blink [seconds]` — flash the LED for `seconds` seconds (default 2)
// so a human can spot which board they're looking at. 2 s at 2 Hz is
// four blinks — enough to register visually without feeling slow.
static void cmdBlink(const char* args) {
    float seconds = 2.0f;
    if (args && *args) seconds = (float)atof(args);
    if (seconds <= 0)   seconds = 2.0f;
    if (seconds > 300)  seconds = 300.0f; // safety cap
    uint32_t ms = (uint32_t)(seconds * 1000);
    StatusLed::beginBlink(ms);
    g_out->printf("blinking for %.1fs\n", seconds);
    ok();
}

// `ledoff [seconds]` — turn the LED off for `seconds` seconds (default 10).
// Paired with the same command sent to every peripheral via `@all` or
// by the GUI, this lets the user spot a frozen board — healthy ones
// go dark, crashed ones stay lit or stuck.
static void cmdLedoff(const char* args) {
    float seconds = 10.0f;
    if (args && *args) seconds = (float)atof(args);
    if (seconds <= 0)   seconds = 10.0f;
    if (seconds > 300)  seconds = 300.0f;
    uint32_t ms = (uint32_t)(seconds * 1000);
    StatusLed::beginDark(ms);
    g_out->printf("led off for %.1fs\n", seconds);
    ok();
}

static void cmdReboot() {
    // No OK — the connection drops. Logged for humans watching the monitor.
    Serial.println(F("# rebooting..."));
    Serial.flush();
    delay(50);
    ESP.restart();
}

// Parse: "pi <idx> <pid> <f>" — single-parameter set.
static void cmdSetParam(char* args) {
    char* t1 = strtok(args, " ");
    char* t2 = strtok(nullptr, " ");
    char* t3 = strtok(nullptr, " ");
    if (!t1 || !t2 || !t3) { err("usage: pi <idx> <pid> <f>"); return; }
    int idx = atoi(t1);
    int pid = atoi(t2);
    float v  = (float)atof(t3);
    Element* el = elementByIndex(idx);
    if (!el) { err("no element at that index"); return; }
    if (!el->setParameter((uint8_t)pid, floatToParam(v))) {
        err("setParameter rejected");
        return;
    }
    ok();
}

// Parse: "p <idx> <f1> [f2 ...]" from args.
static void cmdSetParams(char* args) {
    char* tok = strtok(args, " ");
    if (!tok) { err("usage: p <idx> <f1> [f2 ...]"); return; }
    int idx = atoi(tok);
    Element* el = elementByIndex(idx);
    if (!el) { err("no element at that index"); return; }

    uint16_t vals[8];
    uint8_t n = 0;
    while ((tok = strtok(nullptr, " ")) != nullptr && n < 8) {
        vals[n++] = floatToParam((float)atof(tok));
    }
    if (!el->setParameters(vals, n)) { err("setParameters rejected"); return; }
    ok();
}

// Parse: "demo <idx|all> on|off"
static void cmdDemo(char* args) {
    char* tok = strtok(args, " ");
    if (!tok) { err("usage: demo <idx|all> on|off"); return; }
    bool all = (strcmp(tok, "all") == 0);
    int idx = all ? -1 : atoi(tok);

    char* action = strtok(nullptr, " ");
    if (!action) { err("usage: demo <idx|all> on|off"); return; }
    bool turnOn;
    if      (strcmp(action, "on")  == 0) turnOn = true;
    else if (strcmp(action, "off") == 0) turnOn = false;
    else                                 { err("usage: demo <idx|all> on|off"); return; }

    if (all) {
        for (uint8_t i = 0; i < elementCount; i++) {
            turnOn ? elements[i]->startDemo() : elements[i]->stopDemo();
        }
        g_out->printf("demo all: %s\n", turnOn ? "on" : "off");
    } else {
        Element* el = elementByIndex(idx);
        if (!el) { err("no element at that index"); return; }
        turnOn ? el->startDemo() : el->stopDemo();
        g_out->printf("demo %d: %s\n", idx, turnOn ? "on" : "off");
    }
    ok();
}

static void cmdHelp() { printHelp(); ok(); }

// ------------------------- dispatcher -------------------------

static void handleLine(char* line) {
    aliveAcked = true; // first command received — silence the heartbeat
    while (*line == ' ') line++;
    if (*line == 0) return; // ignore blank lines silently — no OK

    // Diagnostic: surface the length of each command the peripheral
    // actually received. Lets us tell at a glance whether the full
    // `cfg <json>` payload made it through USB CDC without truncation.
    // `# ` prefix so the GUI filters it as a log line.
    Serial.printf("# [cmd] len=%u \"%.16s%s\"\n",
                  (unsigned)strlen(line),
                  line,
                  strlen(line) > 16 ? "…" : "");

    char* sp = strchr(line, ' ');
    char* args = (char*)"";
    if (sp) { *sp = 0; args = sp + 1; }

    if      (!strcmp(line, "whoami"))                         cmdWhoami();
    else if (!strcmp(line, "list"))                           cmdList();
    else if (!strcmp(line, "stop") || !strcmp(line, "s"))     cmdStop(args);
    else if (!strcmp(line, "cfg"))                            cmdCfg(args);
    else if (!strcmp(line, "board"))                          cmdBoard(args);
    else if (!strcmp(line, "reboot"))                         cmdReboot();
    else if (!strcmp(line, "p"))                              cmdSetParams(args);
    else if (!strcmp(line, "pi"))                             cmdSetParam(args);
    else if (!strcmp(line, "demo"))                           cmdDemo(args);
    else if (!strcmp(line, "blink"))                          cmdBlink(args);
    else if (!strcmp(line, "ledoff"))                         cmdLedoff(args);
    else if (!strcmp(line, "help") || !strcmp(line, "?"))     cmdHelp();
    else { g_out->printf("# unknown command: %s (try 'help')\n", line); err("unknown command"); }
}

static void pumpSerial() {
    while (Serial.available()) {
        int c = Serial.read();
        if (c < 0) break;
        if (c == '\r') continue;
        if (c == '\n') {
            cliBuf[cliLen] = 0;
            handleLine(cliBuf);
            cliLen = 0;
            Serial.print(F("> "));
        } else if (cliLen + 1 < CLI_BUF_SIZE) {
            cliBuf[cliLen++] = (char)c;
        }
    }
}

// ------------------------- I2C slave -------------------------
//
// Responds to the three binary messages defined in shared/i2c_messages.json:
//   MSG_GET_CONFIG (0x30) — master writes 1 byte, then reads; we reply
//     with CONFIG_LENGTH (1 byte = element count) followed by one type_id
//     byte per element, in index order.
//   MSG_STOP (0x32) — master writes 1 byte; we allStop() every Element.
//   MSG_SET_PARAMETERS (0x31) — master writes opcode + ELEMENT_INDEX +
//     PARAMETERS_LENGTH + PARAMETERS_DATA. We parse and apply.
//
// Handlers run in the Wire slave task, not from loop(). Keep work short;
// do not use LittleFS or long serial prints. Parameter updates hit _params[]
// directly via setParameters(); the main update() loop picks them up on
// its next tick.
//
// Cross-thread data: element parameter writes are uint16 stores on a
// 32-bit MCU — effectively atomic. The roster of elements[] is built in
// setup() before Wire is enabled, so no concurrent mutation.

// Remembers which message needs a reply next onRequest().
// Written by onI2CReceive, consumed by onI2CRequest. Volatile because
// the two run in different contexts.
static volatile uint8_t pendingResponseType = 0;

// Debug counters — incremented in ISR context, read and printed in loop().
// Tell us whether the I2C slave driver is seeing packets at all.
static volatile uint32_t dbgReceiveCount = 0;
static volatile uint32_t dbgRequestCount = 0;

// ------------------------- Text tunnel state -------------------------
//
// Central streams a text command to us in TEXT_WRITE chunks (offset,
// length, more, data). When the last chunk arrives (more=0) we flag
// textReqReady. The main loop's pumpI2C() sees the flag, swaps g_out
// to write into textRespBuf, runs the command through handleLine(),
// records whether OK or ERR was emitted, then sets textRespLen and
// textRespStatus. Central then polls with TEXT_READ chunks at
// increasing offsets until status != IN_PROGRESS.

static char     textReqBuf[CLI_BUF_SIZE];
static volatile uint16_t textReqLen   = 0;
static volatile bool     textReqReady = false;

static const uint16_t TEXT_RESP_CAP = 4096;
static char     textRespBuf[TEXT_RESP_CAP];
static volatile uint16_t textRespLen    = 0;
static volatile uint8_t  textRespStatus = AOProtocol::TEXT_STATUS_IN_PROGRESS;

// TEXT_READ request staging (set by onReceive, consumed by onRequest).
static volatile uint16_t textReadOffset = 0;
static volatile uint8_t  textReadMaxlen = 0;
static volatile bool     textReadPending = false;

static BufferPrint textResponsePrinter(textRespBuf, TEXT_RESP_CAP);

static void onI2CReceive(int numBytes) {
    dbgReceiveCount++;
    if (numBytes <= 0) return;
    int msg = Wire.read();
    if (msg < 0) return;

    switch ((uint8_t)msg) {
        case MSG_GET_CONFIG:
            pendingResponseType = MSG_GET_CONFIG;
            break;

        case MSG_STOP:
            for (uint8_t i = 0; i < elementCount; i++) {
                elements[i]->allStop();
            }
            break;

        case MSG_SET_PARAMETERS: {
            // Expect: ELEMENT_INDEX (1) + PARAMETERS_LENGTH (1) + data.
            if (Wire.available() < 2) break;
            uint8_t elIdx = (uint8_t)Wire.read();
            uint8_t len   = (uint8_t)Wire.read();
            Element* el   = elementByIndex((int)elIdx);
            // Validate: element must exist, length must match expected
            // count * 2 bytes (uint16 LE per parameter), and enough data
            // must be in the buffer.
            if (!el ||
                len != el->parameterCount() * AOProtocol::PARAM_BYTES ||
                (int)len > Wire.available()) {
                break; // drain handled below
            }
            uint16_t vals[Element::MAX_PARAMS];
            uint8_t n = el->parameterCount();
            for (uint8_t i = 0; i < n; i++) {
                uint8_t lo = (uint8_t)Wire.read();
                uint8_t hi = (uint8_t)Wire.read();
                vals[i] = (uint16_t)lo | ((uint16_t)hi << 8);
            }
            el->setParameters(vals, n);
            break;
        }

        case MSG_SET_PARAMETER: {
            // Expect: ELEMENT_INDEX (1) + PARAMETER_ID (1) + VALUE (2 LE).
            if (Wire.available() < 4) break;
            uint8_t elIdx = (uint8_t)Wire.read();
            uint8_t pid   = (uint8_t)Wire.read();
            uint8_t lo    = (uint8_t)Wire.read();
            uint8_t hi    = (uint8_t)Wire.read();
            Element* el = elementByIndex((int)elIdx);
            if (!el) break;
            uint16_t val = (uint16_t)lo | ((uint16_t)hi << 8);
            el->setParameter(pid, val);
            break;
        }

        case MSG_DEMO: {
            // Expect: ELEMENT_INDEX (1, 0xFF = all) + ACTION (1, 0 = off).
            if (Wire.available() < 2) break;
            uint8_t elIdx  = (uint8_t)Wire.read();
            uint8_t action = (uint8_t)Wire.read();
            bool on = (action != 0);
            if (elIdx == DEMO_ALL_ELEMENTS) {
                for (uint8_t i = 0; i < elementCount; i++) {
                    on ? elements[i]->startDemo() : elements[i]->stopDemo();
                }
            } else {
                Element* el = elementByIndex((int)elIdx);
                if (el) (on ? el->startDemo() : el->stopDemo());
            }
            break;
        }

        case MSG_TEXT_WRITE: {
            // Expect: OFFSET (2 LE) + LENGTH (1) + MORE (1) + DATA.
            if (Wire.available() < 4) break;
            uint8_t olo = (uint8_t)Wire.read();
            uint8_t ohi = (uint8_t)Wire.read();
            uint16_t offset = (uint16_t)olo | ((uint16_t)ohi << 8);
            uint8_t length  = (uint8_t)Wire.read();
            uint8_t more    = (uint8_t)Wire.read();

            // First chunk of a new request? Reset state.
            if (offset == 0) {
                textReqLen      = 0;
                textReqReady    = false;
                textRespLen     = 0;
                textRespStatus  = TEXT_STATUS_IN_PROGRESS;
            }

            // Copy payload into request buffer, bounds-checked.
            for (uint8_t i = 0; i < length; i++) {
                if (Wire.available() <= 0) break;
                uint8_t b = (uint8_t)Wire.read();
                uint16_t pos = offset + i;
                if (pos < CLI_BUF_SIZE - 1) textReqBuf[pos] = (char)b;
            }
            uint16_t end = offset + length;
            if (end > textReqLen) textReqLen = end;
            if (textReqLen > CLI_BUF_SIZE - 1) textReqLen = CLI_BUF_SIZE - 1;

            if (!more) {
                textReqBuf[textReqLen] = 0; // null-terminate for handleLine
                textReqReady = true;        // main loop pumpI2C() will act
            }
            break;
        }

        case MSG_TEXT_READ: {
            // Expect: OFFSET (2 LE) + MAXLEN (1). Stage for onI2CRequest().
            if (Wire.available() < 3) break;
            uint8_t olo = (uint8_t)Wire.read();
            uint8_t ohi = (uint8_t)Wire.read();
            uint16_t offset = (uint16_t)olo | ((uint16_t)ohi << 8);
            uint8_t maxlen  = (uint8_t)Wire.read();
            if (maxlen > TEXT_CHUNK_MAX) maxlen = TEXT_CHUNK_MAX;
            textReadOffset  = offset;
            textReadMaxlen  = maxlen;
            textReadPending = true;
            pendingResponseType = MSG_TEXT_READ;
            break;
        }

        default:
            // Unknown opcode — drop silently.
            break;
    }

    // Drain any residual bytes so the receive FIFO is empty for the next
    // transaction.
    while (Wire.available() > 0) (void)Wire.read();
}

static void onI2CRequest() {
    dbgRequestCount++;
    switch (pendingResponseType) {
        case MSG_GET_CONFIG: {
            // CONFIG_LENGTH byte then one type_id byte per Element, in
            // per-board index order (same order as config.json).
            Wire.write((uint8_t)elementCount);
            for (uint8_t i = 0; i < elementCount; i++) {
                Wire.write(elements[i]->typeId());
            }
            break;
        }
        case MSG_TEXT_READ: {
            // Response layout: LENGTH (1) + MORE (1) + STATUS (1) + DATA.
            // If the command hasn't finished yet, report the bytes we
            // have so far with STATUS=IN_PROGRESS and MORE=1.
            uint16_t off    = textReadOffset;
            uint16_t maxlen = textReadMaxlen;
            uint16_t avail  = (off < textRespLen) ? (textRespLen - off) : 0;
            uint16_t send   = (avail < maxlen) ? avail : maxlen;
            uint8_t  more   = (off + send < textRespLen) ? 1 : 0;

            Wire.write((uint8_t)send);
            Wire.write(more);
            Wire.write(textRespStatus);
            for (uint16_t i = 0; i < send; i++) {
                Wire.write((uint8_t)textRespBuf[off + i]);
            }
            textReadPending = false;
            break;
        }
        default:
            // Master read with no command staged — keep the bus safe
            // by sending a zero byte.
            Wire.write((uint8_t)0);
            break;
    }
    pendingResponseType = 0;
}

// I2C slave pins. GPIO 8 (SDA) / GPIO 9 (SCL) are the ESP32-S3 variant
// defaults and the only pins confirmed to work for Wire slave on this board.
// Override with -DI2C_SDA_PIN_GPIO=<n> / -DI2C_SCL_PIN_GPIO=<n> in
// platformio.ini.  Wire.setPins() is used (not Wire.begin(addr, sda, scl))
// to avoid the IDF GPIO-state conflict that hangs the slave driver.
// Central uses the same GPIO 8/9 — consistent wiring across all boards.
#ifndef I2C_SDA_PIN_GPIO
#  define I2C_SDA_PIN_GPIO 8
#endif
#ifndef I2C_SCL_PIN_GPIO
#  define I2C_SCL_PIN_GPIO 9
#endif
static const int I2C_SDA_PIN = I2C_SDA_PIN_GPIO;
static const int I2C_SCL_PIN = I2C_SCL_PIN_GPIO;

// Called from loop() — if a text-tunnel request has been fully received,
// run it through the command dispatcher now (main-loop context, where
// LittleFS access and long work are safe).
static void pumpI2C() {
    if (!textReqReady) return;
    textReqReady = false;

    // Redirect command output into the response buffer and reset flags.
    textResponsePrinter.reset();
    textRespLen    = 0;
    textRespStatus = TEXT_STATUS_IN_PROGRESS;
    cmdEmittedOk   = false;
    cmdEmittedErr  = false;

    Print* savedOut = g_out;
    g_out = &textResponsePrinter;

    handleLine(textReqBuf);

    // handleLine silently returns on blank/whitespace input. Emit a
    // synthetic ERR in that case so the tunnel response is complete
    // (every forwarded command must land with OK or ERR as its last
    // line, or the host side has no way to terminate its read loop).
    if (!cmdEmittedOk && !cmdEmittedErr) {
        textResponsePrinter.println(F("ERR empty command"));
        cmdEmittedErr = true;
    }

    g_out = savedOut;

    // Finalise: publish length first, then flip status so a concurrent
    // TEXT_READ reads them in a safe order.
    textRespLen    = (uint16_t)textResponsePrinter.size();
    textRespStatus = cmdEmittedErr ? TEXT_STATUS_ERR : TEXT_STATUS_OK;
}

static void setupI2CSlave() {
    // Address must be in the I2C 7-bit "safe" range. Anything else is
    // either reserved (0x00-0x07, 0x78-0x7F) or outside our project-wide
    // convention.
    if (board.i2cAddress < 0x08 || board.i2cAddress > 0x77) {
        Serial.printf("# [I2C] skipping slave setup — address %u out of range\n",
                      board.i2cAddress);
        return;
    }

    // Use Wire.setPins() to store our pin choice, then Wire.begin(addr)
    // with no explicit pin arguments.  Calling Wire.begin(addr, sda, scl)
    // with explicit GPIOs causes a hang on arduino-esp32 v2.x because the
    // IDF's i2cSlaveInit() conflicts with whatever GPIO-matrix state
    // Arduino's initPins() left behind.  Wire.setPins() only stores the
    // pin numbers in the Wire object (no hardware touch); the GPIO hardware
    // is then configured correctly by i2cSlaveInit() inside begin().
    Wire.setPins(I2C_SDA_PIN, I2C_SCL_PIN);

    Serial.printf("# [I2C] free heap: %u  free PSRAM: %u\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getFreePsram());
    Serial.printf("# [I2C] calling Wire.begin(0x%02X) SDA=GPIO%d SCL=GPIO%d...\n",
                  board.i2cAddress, I2C_SDA_PIN, I2C_SCL_PIN);
    Serial.flush(); // ensure the lines above appear even if begin() hangs

    bool ok = Wire.begin((uint8_t)board.i2cAddress);

    if (!ok) {
        Serial.printf("# [I2C] Wire.begin() FAILED "
                      "(SDA=GPIO%d SCL=GPIO%d addr=0x%02X)\n",
                      I2C_SDA_PIN, I2C_SCL_PIN, board.i2cAddress);
        return;
    }

    Wire.onReceive(onI2CReceive);
    Wire.onRequest(onI2CRequest);

    Serial.printf("# [I2C] slave started at 0x%02X (SDA=GPIO%d, SCL=GPIO%d)\n",
                  board.i2cAddress, I2C_SDA_PIN, I2C_SCL_PIN);
}

// Bring up hardware for each configured Element. Elements that fail to
// initialise are removed from the array and deleted.
static void initialiseElements() {
    uint8_t writeIdx = 0;
    for (uint8_t readIdx = 0; readIdx < elementCount; readIdx++) {
        Element* el = elements[readIdx];
        if (el->initialise()) {
            elements[writeIdx++] = el;
        } else {
            Serial.printf("# [main] initialise failed for %s, removing\n",
                          el->typeName());
            delete el;
        }
    }
    elementCount = writeIdx;
}

// ------------------------- Arduino entry points -------------------------

void setup() {
    // USBCDC (S3): setRxBufferSize must come BEFORE begin() — begin()
    // allocates the default 256-byte ring buffer and a later resize is a
    // no-op.  256 bytes < one `cfg <json>` payload → truncated JSON.
    Serial.setRxBufferSize(CLI_BUF_SIZE);
    Serial.begin(115200);

    // Wait up to 3 s for the USB-CDC port to be opened by the host.
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 3000) delay(10);
    delay(200);

    // Status LED up. White during boot (so a long boot or a hang is
    // visually distinct from a successful green); flipped to role green
    // at the end of setup() once everything is initialised.
    StatusLed::begin();
    StatusLed::setSolid(255, 255, 255);

    Serial.println();
    Serial.println(F("# =============================="));
    Serial.println(F("# === AO ELECTRONICS PERIPHERAL ==="));
    Serial.println(F("# =============================="));

    if (!ConfigLoader::mount()) {
        Serial.println(F("# LittleFS unavailable; running with no Elements"));
    } else {
        ConfigLoader::loadBoardConfig(board);
        Serial.printf("# i2c_address = 0x%02X\n", board.i2cAddress);
        ConfigLoader::loadElements(elements, MAX_ELEMENTS_PER_BOARD, elementCount);
        initialiseElements();
    }

    // Bring up the I2C slave AFTER elements are loaded + initialised, so a
    // Central that sends GET_CONFIG right after boot gets a real answer.
    setupI2CSlave();

    // Boot complete — flip the LED to role green. Stays until something
    // else overrides (blink / ledoff / save-pulse).
    StatusLed::setSolid(0, 255, 0);

    lastUpdateMs = millis();
    Serial.print(F("> "));
}

void loop() {
    uint32_t now = millis();
    uint32_t dt = now - lastUpdateMs;
    lastUpdateMs = now;

    // Startup heartbeat: repeated every 2 s until the first command is
    // received.  Even if boot messages were missed (USB-CDC open race),
    // the user sees the board is alive within 2 s of opening a monitor.
    if (!aliveAcked) {
        static uint32_t lastAliveMs = 0;
        if (now - lastAliveMs >= 2000) {
            Serial.println(F("# alive — type 'help' for commands"));
            Serial.print(F("> "));
            lastAliveMs = now;
        }
    }

    // I2C callback debug: print whenever the ISR counters advance.
    {
        static uint32_t lastRx = 0, lastReq = 0;
        uint32_t rx  = dbgReceiveCount;
        uint32_t req = dbgRequestCount;
        if (rx != lastRx || req != lastReq) {
            Serial.printf("# [I2C dbg] receive=%u request=%u\n",
                          (unsigned)rx, (unsigned)req);
            lastRx = rx; lastReq = req;
        }
    }

    pumpSerial();
    pumpI2C();
    for (uint8_t i = 0; i < elementCount; i++) {
        elements[i]->update(dt);
    }
    StatusLed::update(now);

    delay(LOOP_DELAY_MS);
}
