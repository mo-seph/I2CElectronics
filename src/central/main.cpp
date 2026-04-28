// Central main.cpp
//
// Central device firmware — M0 + M1.
//
// M0: Serial CLI with the same framing conventions as the Peripheral
//     (OK / ERR terminator, `# ` log prefix, `> ` prompt).
//     Commands: whoami, help|?, reboot, (unknown → ERR).
//
// M1: I2C master scan + roster. A simple flat array keeps track of
//     Peripherals seen at addresses 0x08..0x27 (decimal 8..39). On first
//     sight of a new address, fetchConfig() pulls the type_id list via
//     MSG_GET_CONFIG. Three consecutive misses mark an entry stale;
//     six removes it from the roster.
//     Commands: scan, list, forget <addr>, scan_interval <ms>.
//
// Framing / style rules (mirroring src/peripheral/main.cpp):
//   - Each command ends with exactly one `OK` or `ERR <reason>` line.
//   - Diagnostic log lines always start with `# `.
//   - The prompt `> ` is emitted after each command for human UX.
//   - Addresses are always printed as decimal integers.
//   - No dynamic allocation; plain C-style dispatch with strtok/atoi/strcmp.
//
// Protocol: see shared/serial_protocol.md and shared/i2c_messages.json.

#include <Arduino.h>
#include <Wire.h>

#include "Protocol.h"
#include "StatusLed.h"

using namespace AOProtocol;

// ------------------------- Tuning -------------------------

// Central has no per-element update loop; keep the loop tight so that USB
// CDC responds quickly. 5 ms is the value the plan specifies.
static const uint32_t LOOP_DELAY_MS = 5;

// I2C master bus speed. 100 kHz is conservative; bump later if stable.
static const uint32_t I2C_FREQ_HZ = 100000;

// I2C pins. Override with -DI2C_SDA_PIN_GPIO=<n> / -DI2C_SCL_PIN_GPIO=<n>
// in platformio.ini build_flags.  Defaults match the Waveshare ESP32-S3-Zero
// header (GPIO 8 = SDA, GPIO 9 = SCL); change the build flags if those pins
// are in use for other purposes or show signs of damage.
#ifndef I2C_SDA_PIN_GPIO
#  define I2C_SDA_PIN_GPIO 8
#endif
#ifndef I2C_SCL_PIN_GPIO
#  define I2C_SCL_PIN_GPIO 9
#endif
static const int I2C_SDA_PIN = I2C_SDA_PIN_GPIO;
static const int I2C_SCL_PIN = I2C_SCL_PIN_GPIO;

// Address window that Peripherals live in (signed-off decision — see
// CENTRAL_PLAN.md).
static const uint8_t  I2C_SCAN_MIN_ADDR = 0x08; // 8
static const uint8_t  I2C_SCAN_MAX_ADDR = 0x27; // 39 inclusive

// Roster limits.
static const uint8_t  MAX_PERIPHERALS = 32;

// Miss thresholds (consecutive failed scans).
static const uint8_t  STALE_THRESHOLD  = 3; // display as stale
static const uint8_t  FORGET_THRESHOLD = 6; // remove from roster

// Firmware version string reported by whoami.
static const char*    FW_VERSION = "v0.1";

// ------------------------- Roster -------------------------

struct PeripheralRec {
    uint8_t  address;
    uint32_t lastSeenMs;
    uint32_t lastMissCount; // consecutive scan misses
    uint8_t  typeCount;
    uint8_t  typeIds[MAX_ELEMENTS_PER_BOARD];
    bool     configFetched;
    bool     inUse;
};

static PeripheralRec roster[MAX_PERIPHERALS];

// Periodic-scan cadence. 0 = off (default, per signed-off decision).
static uint32_t scanIntervalMs = 0;
static uint32_t lastScanMs     = 0;

// ------------------------- CLI state -------------------------

static const uint16_t CLI_BUF_SIZE = 4096;
static char           cliBuf[CLI_BUF_SIZE];
static uint16_t       cliLen = 0;
// Cleared on the first command received. Drives a heartbeat in loop()
// so the board's liveness is visible even when boot messages are missed.
static bool           aliveAcked = false;

// ------------------------- tiny helpers -------------------------

static void ok()                 { Serial.println(F("OK")); }
static void err(const char* msg) { Serial.printf("ERR %s\n", msg); }

// Count current in-use roster entries.
static uint8_t rosterCount() {
    uint8_t n = 0;
    for (uint8_t i = 0; i < MAX_PERIPHERALS; i++) if (roster[i].inUse) n++;
    return n;
}

// Find a roster slot for the given address, or -1 if not present.
static int rosterFind(uint8_t addr) {
    for (uint8_t i = 0; i < MAX_PERIPHERALS; i++) {
        if (roster[i].inUse && roster[i].address == addr) return (int)i;
    }
    return -1;
}

// Claim a free slot for a new address, or -1 if full.
static int rosterAlloc(uint8_t addr) {
    for (uint8_t i = 0; i < MAX_PERIPHERALS; i++) {
        if (!roster[i].inUse) {
            roster[i].address       = addr;
            roster[i].lastSeenMs    = millis();
            roster[i].lastMissCount = 0;
            roster[i].typeCount     = 0;
            roster[i].configFetched = false;
            roster[i].inUse         = true;
            return (int)i;
        }
    }
    return -1;
}

static void rosterRelease(int slot) {
    if (slot < 0 || slot >= MAX_PERIPHERALS) return;
    roster[slot].inUse         = false;
    roster[slot].typeCount     = 0;
    roster[slot].configFetched = false;
}

// ------------------------- I2C helpers -------------------------

// Ask `addr` for its GET_CONFIG_RESPONSE. Fills rec.typeIds[] and sets
// configFetched = true on success. Leaves configFetched = false on any
// failure (bus NAK, zero bytes returned, etc.).
static void fetchConfig(PeripheralRec& rec) {
    uint8_t addr = rec.address;

    Wire.beginTransmission(addr);
    Wire.write(MSG_GET_CONFIG);
    uint8_t txStatus = Wire.endTransmission();
    if (txStatus != 0) {
        Serial.printf("# [cfg] addr=%u GET_CONFIG write failed (status=%u)\n",
                      addr, txStatus);
        return;
    }

    // We want one length byte then up to MAX_ELEMENTS_PER_BOARD type bytes.
    uint8_t want = 1 + MAX_ELEMENTS_PER_BOARD;
    uint8_t got  = Wire.requestFrom((int)addr, (int)want);
    if (got == 0) {
        Serial.printf("# [cfg] addr=%u GET_CONFIG no response\n", addr);
        return;
    }

    int first = Wire.read();
    if (first < 0) {
        Serial.printf("# [cfg] addr=%u GET_CONFIG empty response\n", addr);
        return;
    }
    uint8_t configLength = (uint8_t)first;
    if (configLength > MAX_ELEMENTS_PER_BOARD) {
        Serial.printf("# [cfg] addr=%u GET_CONFIG length %u exceeds max %u\n",
                      addr, configLength, MAX_ELEMENTS_PER_BOARD);
        configLength = MAX_ELEMENTS_PER_BOARD;
    }

    uint8_t readTypes = 0;
    while (readTypes < configLength && Wire.available() > 0) {
        int b = Wire.read();
        if (b < 0) break;
        rec.typeIds[readTypes++] = (uint8_t)b;
    }
    // Drain any extra bytes the slave returned (we over-requested).
    while (Wire.available() > 0) (void)Wire.read();

    rec.typeCount     = readTypes;
    rec.configFetched = (readTypes == configLength);

    if (!rec.configFetched) {
        Serial.printf("# [cfg] addr=%u GET_CONFIG short read (%u/%u)\n",
                      addr, readTypes, configLength);
    } else {
        Serial.printf("# [cfg] addr=%u types=", addr);
        for (uint8_t i = 0; i < rec.typeCount; i++) {
            Serial.printf("%s%u", i ? "," : "", rec.typeIds[i]);
        }
        Serial.println();
    }
}

// Probe one address with a zero-length write. ACK (status 0) means present.
static bool probeAddress(uint8_t addr) {
    Wire.beginTransmission(addr);
    return (Wire.endTransmission() == 0);
}

// Sweep the configured address range; update roster; log a one-line summary.
static void doScan() {
    uint32_t now = millis();

    // First: mark present/absent for each slot.
    bool presentThisScan[MAX_PERIPHERALS];
    for (uint8_t i = 0; i < MAX_PERIPHERALS; i++) presentThisScan[i] = false;

    // Track freshly-added addresses so we can fetchConfig AFTER the sweep
    // completes — keeps per-address probe timing predictable.
    uint8_t newlyAdded[MAX_PERIPHERALS];
    uint8_t newlyAddedCount = 0;

    uint8_t foundAddrs[MAX_PERIPHERALS];
    uint8_t foundCount = 0;

    for (uint8_t addr = I2C_SCAN_MIN_ADDR; addr <= I2C_SCAN_MAX_ADDR; addr++) {
        if (!probeAddress(addr)) continue;

        if (foundCount < MAX_PERIPHERALS) foundAddrs[foundCount++] = addr;

        int slot = rosterFind(addr);
        if (slot < 0) {
            slot = rosterAlloc(addr);
            if (slot < 0) {
                Serial.printf("# [scan] roster full, ignoring %u\n", addr);
                continue;
            }
            if (newlyAddedCount < MAX_PERIPHERALS) {
                newlyAdded[newlyAddedCount++] = (uint8_t)slot;
            }
        } else {
            roster[slot].lastSeenMs    = now;
            roster[slot].lastMissCount = 0;
        }
        presentThisScan[slot] = true;
    }

    // Bump miss counts for entries not seen this scan; drop the long-gone.
    for (uint8_t i = 0; i < MAX_PERIPHERALS; i++) {
        if (!roster[i].inUse)    continue;
        if (presentThisScan[i])  continue;
        roster[i].lastMissCount++;
        if (roster[i].lastMissCount >= FORGET_THRESHOLD) {
            Serial.printf("# [scan] forgetting %u (missed %u scans)\n",
                          roster[i].address, roster[i].lastMissCount);
            rosterRelease((int)i);
        }
    }

    // Fetch config for genuinely new peripherals now that the sweep is done.
    for (uint8_t i = 0; i < newlyAddedCount; i++) {
        fetchConfig(roster[newlyAdded[i]]);
    }

    // Summary line.
    Serial.print(F("# [scan] found"));
    if (foundCount == 0) {
        Serial.print(F(" (none)"));
    } else {
        for (uint8_t i = 0; i < foundCount; i++) {
            Serial.printf(" %u", foundAddrs[i]);
        }
    }
    Serial.println();

    lastScanMs = now;
}

// ------------------------- commands -------------------------

static void printHelp() {
    Serial.println(F(
        "Central commands:\n"
        "  whoami                    - device role + peripheral count\n"
        "  list                      - list peripherals in roster\n"
        "  scan                      - scan I2C bus now\n"
        "  scan_interval <ms>        - periodic scan cadence (0 = off)\n"
        "  forget <addr>             - remove address from roster\n"
        "  reboot                    - restart this device\n"
        "  blink [seconds]           - flash this Central's LED\n"
        "  ledoff [seconds]          - Central LED off (ID crashed boards)\n"
        "  help | ?                  - show this help\n"
        "Addressed over I2C (binary hot path):\n"
        "  @<addr> stop              - stop all elements on one peripheral\n"
        "  @<addr> p <idx> <f..>     - set all params on an element\n"
        "  @<addr> pi <idx> <pid> <f>- set one param on an element\n"
        "  @<addr> demo <idx|all> on|off\n"
        "  @all stop                 - fan-out STOP to every peripheral\n"
        "  @all demo on|off          - fan-out demo toggle\n"
        "Addressed via text tunnel (any text command on that peripheral):\n"
        "  @<addr> whoami | list | help | reboot | stop <idx>\n"
        "  @<addr> cfg [<json>]      - read or write element config\n"
        "  @<addr> board [<json>]    - read or write board settings"
    ));
}

static void cmdWhoami() {
    Serial.printf("WHOAMI role=central peripherals=%u fw=%s\n",
                  rosterCount(), FW_VERSION);
    ok();
}

static void cmdHelp() { printHelp(); ok(); }

static void cmdReboot() {
    // Match the Peripheral's reboot: no OK, connection drops.
    Serial.println(F("# rebooting..."));
    Serial.flush();
    delay(50);
    ESP.restart();
}

static void cmdScan() {
    doScan();
    ok();
}

static void cmdList() {
    uint32_t now = millis();
    for (uint8_t i = 0; i < MAX_PERIPHERALS; i++) {
        if (!roster[i].inUse) continue;
        PeripheralRec& r = roster[i];
        uint32_t ageMs = now - r.lastSeenMs;

        Serial.printf("%u last_seen=%ums types=[", r.address, (unsigned)ageMs);
        for (uint8_t t = 0; t < r.typeCount; t++) {
            Serial.printf("%s%u", t ? "," : "", r.typeIds[t]);
        }
        Serial.print(F("] configFetched="));
        Serial.print(r.configFetched ? F("yes") : F("no"));
        if (r.lastMissCount >= STALE_THRESHOLD) Serial.print(F(" stale"));
        Serial.println();
    }
    ok();
}

static void cmdForget(const char* args) {
    if (!args || !*args) { err("usage: forget <addr>"); return; }
    int addr = atoi(args);
    if (addr < 0 || addr > 127) { err("invalid address"); return; }
    int slot = rosterFind((uint8_t)addr);
    if (slot >= 0) {
        rosterRelease(slot);
        Serial.printf("# forgot %d\n", addr);
    } else {
        // Idempotent: success even if not present.
        Serial.printf("# %d not in roster\n", addr);
    }
    ok();
}

static void cmdBlink(const char* args) {
    float seconds = 2.0f;
    if (args && *args) seconds = (float)atof(args);
    if (seconds <= 0)   seconds = 2.0f;
    if (seconds > 300)  seconds = 300.0f;
    StatusLed::beginBlink((uint32_t)(seconds * 1000));
    Serial.printf("blinking for %.1fs\n", seconds);
    ok();
}

static void cmdLedoff(const char* args) {
    float seconds = 10.0f;
    if (args && *args) seconds = (float)atof(args);
    if (seconds <= 0)   seconds = 10.0f;
    if (seconds > 300)  seconds = 300.0f;
    StatusLed::beginDark((uint32_t)(seconds * 1000));
    Serial.printf("led off for %.1fs\n", seconds);
    ok();
}

static void cmdScanInterval(const char* args) {
    if (!args || !*args) { err("usage: scan_interval <ms>"); return; }
    long ms = atol(args);
    if (ms < 0) { err("interval must be >= 0"); return; }
    scanIntervalMs = (uint32_t)ms;
    lastScanMs = millis(); // reset the clock so the first tick is one interval away
    if (scanIntervalMs == 0) {
        Serial.println(F("# periodic scan disabled"));
    } else {
        Serial.printf("# periodic scan every %u ms\n", (unsigned)scanIntervalMs);
    }
    ok();
}

// ------------------------- addressed / forwarded commands -------------------------
//
// `@<addr> <cmd> <args>` peels the address and dispatches to a forwarder
// that builds the matching binary I2C message. `@all <cmd> <args>` fans
// out to every roster entry (fire-and-forget — per-device NAK logged via
// `# ` but doesn't fail the overall command).
//
// Only realtime commands are handled in binary here: stop, p, pi, demo.
// Config read/write, whoami, reboot etc. will land when the M3/M4 text
// tunnel ships.

// Map Wire.endTransmission() return codes into short strings.
static const char* busErrStr(uint8_t s) {
    switch (s) {
        case 0: return "ok";
        case 1: return "data too long";
        case 2: return "nak addr";
        case 3: return "nak data";
        case 4: return "other";
        case 5: return "timeout";
        default: return "unknown";
    }
}

// Bump miss count so a flaky peripheral gets marked stale quickly.
static void bumpMissCount(uint8_t addr) {
    int slot = rosterFind(addr);
    if (slot >= 0) roster[slot].lastMissCount++;
}

// (floatToParam comes from AOProtocol via Protocol.h — no local copy needed.)

// --- forwarders (addr must be in roster; caller validates) ---

static bool fwdStop(uint8_t addr) {
    Wire.beginTransmission(addr);
    Wire.write(MSG_STOP);
    uint8_t s = Wire.endTransmission();
    if (s != 0) {
        Serial.printf("# [fwd] stop addr=%u %s\n", addr, busErrStr(s));
        bumpMissCount(addr);
        err(busErrStr(s));
        return false;
    }
    return true;
}

// "p <idx> <f1> [f2 ...]" — all parameters for one element.
static bool fwdSetParameters(uint8_t addr, char* args) {
    char* tok = strtok(args, " ");
    if (!tok) { err("usage: p <idx> <f1> [f2 ...]"); return false; }
    int elIdx = atoi(tok);
    if (elIdx < 0 || elIdx > 255) { err("bad element index"); return false; }

    uint16_t vals[8];
    uint8_t n = 0;
    while ((tok = strtok(nullptr, " ")) != nullptr && n < 8) {
        vals[n++] = floatToParam((float)atof(tok));
    }
    if (n == 0) { err("need at least one value"); return false; }

    Wire.beginTransmission(addr);
    Wire.write(MSG_SET_PARAMETERS);
    Wire.write((uint8_t)elIdx);
    Wire.write((uint8_t)(n * 2)); // length in bytes
    for (uint8_t i = 0; i < n; i++) {
        Wire.write((uint8_t)(vals[i] & 0xFF));
        Wire.write((uint8_t)(vals[i] >> 8));
    }
    uint8_t s = Wire.endTransmission();
    if (s != 0) {
        Serial.printf("# [fwd] p addr=%u %s\n", addr, busErrStr(s));
        bumpMissCount(addr);
        err(busErrStr(s));
        return false;
    }
    return true;
}

// "pi <idx> <pid> <f>" — single parameter on one element.
static bool fwdSetParameter(uint8_t addr, char* args) {
    char* t1 = strtok(args, " ");
    char* t2 = strtok(nullptr, " ");
    char* t3 = strtok(nullptr, " ");
    if (!t1 || !t2 || !t3) { err("usage: pi <idx> <pid> <f>"); return false; }
    int elIdx = atoi(t1);
    int pid   = atoi(t2);
    uint16_t val = floatToParam((float)atof(t3));

    Wire.beginTransmission(addr);
    Wire.write(MSG_SET_PARAMETER);
    Wire.write((uint8_t)elIdx);
    Wire.write((uint8_t)pid);
    Wire.write((uint8_t)(val & 0xFF));
    Wire.write((uint8_t)(val >> 8));
    uint8_t s = Wire.endTransmission();
    if (s != 0) {
        Serial.printf("# [fwd] pi addr=%u %s\n", addr, busErrStr(s));
        bumpMissCount(addr);
        err(busErrStr(s));
        return false;
    }
    return true;
}

// "demo <idx|all> on|off" — element demo toggle.
static bool fwdDemo(uint8_t addr, char* args) {
    char* t1 = strtok(args, " ");
    char* t2 = strtok(nullptr, " ");
    if (!t1 || !t2) { err("usage: demo <idx|all> on|off"); return false; }
    uint8_t elIdx;
    if (!strcmp(t1, "all")) elIdx = DEMO_ALL_ELEMENTS;
    else {
        int i = atoi(t1);
        if (i < 0 || i > 254) { err("bad element index"); return false; }
        elIdx = (uint8_t)i;
    }
    uint8_t action;
    if      (!strcmp(t2, "on"))  action = 1;
    else if (!strcmp(t2, "off")) action = 0;
    else { err("usage: demo <idx|all> on|off"); return false; }

    Wire.beginTransmission(addr);
    Wire.write(MSG_DEMO);
    Wire.write(elIdx);
    Wire.write(action);
    uint8_t s = Wire.endTransmission();
    if (s != 0) {
        Serial.printf("# [fwd] demo addr=%u %s\n", addr, busErrStr(s));
        bumpMissCount(addr);
        err(busErrStr(s));
        return false;
    }
    return true;
}

// --- @all fan-out (fire-and-forget) ---

static void fanOutStop() {
    // Fire-and-forget per signed-off decision. Inline the I2C send
    // rather than calling fwdStop() so per-peripheral bus errors don't
    // emit ERR lines (they'd break the single-OK framing).
    uint8_t nSent = 0;
    for (uint8_t i = 0; i < MAX_PERIPHERALS; i++) {
        if (!roster[i].inUse) continue;
        uint8_t addr = roster[i].address;
        Wire.beginTransmission(addr);
        Wire.write(MSG_STOP);
        uint8_t s = Wire.endTransmission();
        if (s != 0) {
            Serial.printf("# [@all stop] addr=%u %s\n", addr, busErrStr(s));
            bumpMissCount(addr);
        }
        nSent++;
    }
    Serial.printf("# [@all] stop sent to %u peripheral(s)\n", nSent);
    ok();
}

static void fanOutDemo(char* args) {
    char* t1 = strtok(args, " ");
    if (!t1) { err("usage: @all demo on|off"); return; }
    uint8_t action;
    if      (!strcmp(t1, "on"))  action = 1;
    else if (!strcmp(t1, "off")) action = 0;
    else { err("usage: @all demo on|off"); return; }

    uint8_t nSent = 0;
    for (uint8_t i = 0; i < MAX_PERIPHERALS; i++) {
        if (!roster[i].inUse) continue;
        uint8_t addr = roster[i].address;
        Wire.beginTransmission(addr);
        Wire.write(MSG_DEMO);
        Wire.write(DEMO_ALL_ELEMENTS);
        Wire.write(action);
        uint8_t s = Wire.endTransmission();
        if (s != 0) {
            Serial.printf("# [@all demo] addr=%u %s\n", addr, busErrStr(s));
            bumpMissCount(addr);
        }
        nSent++;
    }
    Serial.printf("# [@all] demo %s sent to %u peripheral(s)\n",
                  action ? "on" : "off", nSent);
    ok();
}

// --- Text tunnel ---------------------------------------------------------
//
// forwardText() carries a full text command + response between Central's
// own USB Serial and the addressed Peripheral's command dispatcher, over
// the I2C binary TEXT_WRITE / TEXT_READ messages. Non-realtime commands
// (cfg, board, whoami, reboot, help, stop <idx>) ride this path — the
// OK/ERR terminator is embedded in the peripheral's own response, so
// forwardText() does NOT emit its own on success.
//
// Return value: true if the response was fully forwarded (caller must
// not emit another OK/ERR — the response embeds it). false if a bus
// error or timeout — err() was already emitted inside.

static const uint32_t TEXT_TUNNEL_TIMEOUT_MS = 5000;
static const uint32_t TEXT_TUNNEL_POLL_MS    = 10;

static bool sendTextWriteChunk(uint8_t addr, uint16_t offset, const char* buf,
                               uint8_t len, bool more) {
    Wire.beginTransmission(addr);
    Wire.write(MSG_TEXT_WRITE);
    Wire.write((uint8_t)(offset & 0xFF));
    Wire.write((uint8_t)((offset >> 8) & 0xFF));
    Wire.write(len);
    Wire.write((uint8_t)(more ? 1 : 0));
    for (uint8_t i = 0; i < len; i++) Wire.write((uint8_t)buf[i]);
    uint8_t s = Wire.endTransmission();
    if (s != 0) {
        Serial.printf("# [tunnel] addr=%u TEXT_WRITE %s\n", addr, busErrStr(s));
        bumpMissCount(addr);
        return false;
    }
    return true;
}

static bool forwardText(uint8_t addr, const char* line) {
    size_t len = strlen(line);

    // 1) Stream the request in TEXT_CHUNK_MAX-byte chunks.
    //    Edge case: empty line — send one chunk with length=0, more=0.
    uint16_t sent = 0;
    if (len == 0) {
        if (!sendTextWriteChunk(addr, 0, line, 0, /*more=*/false)) {
            err("bus");
            return false;
        }
    } else {
        while (sent < len) {
            uint8_t chunk = (uint8_t)((len - sent > TEXT_CHUNK_MAX) ? TEXT_CHUNK_MAX : (len - sent));
            bool more = (sent + chunk < len);
            if (!sendTextWriteChunk(addr, sent, line + sent, chunk, more)) {
                err("bus");
                return false;
            }
            sent += chunk;
        }
    }

    // 2) Poll with TEXT_READ chunks, streaming each one to Central's
    //    USB Serial, until STATUS != IN_PROGRESS and no more bytes left.
    uint16_t readOffset = 0;
    uint32_t t0 = millis();
    while (millis() - t0 < TEXT_TUNNEL_TIMEOUT_MS) {
        // Stage the read request.
        Wire.beginTransmission(addr);
        Wire.write(MSG_TEXT_READ);
        Wire.write((uint8_t)(readOffset & 0xFF));
        Wire.write((uint8_t)((readOffset >> 8) & 0xFF));
        Wire.write(TEXT_CHUNK_MAX);
        uint8_t s = Wire.endTransmission();
        if (s != 0) {
            Serial.printf("# [tunnel] addr=%u TEXT_READ req %s\n", addr, busErrStr(s));
            bumpMissCount(addr);
            err("bus");
            return false;
        }

        // Read LENGTH + MORE + STATUS + up to TEXT_CHUNK_MAX data bytes.
        uint8_t got = Wire.requestFrom((int)addr, (int)(3 + TEXT_CHUNK_MAX));
        if (got < 3) {
            Serial.printf("# [tunnel] addr=%u TEXT_READ short (%u)\n", addr, got);
            delay(TEXT_TUNNEL_POLL_MS);
            continue;
        }
        uint8_t length = (uint8_t)Wire.read();
        uint8_t more   = (uint8_t)Wire.read();
        uint8_t status = (uint8_t)Wire.read();

        // Stream data bytes straight to Central's USB Serial.
        for (uint8_t i = 0; i < length && Wire.available() > 0; i++) {
            Serial.write((uint8_t)Wire.read());
        }
        // Drain any extras the peripheral padded into the request size.
        while (Wire.available() > 0) (void)Wire.read();

        readOffset += length;

        if (status != TEXT_STATUS_IN_PROGRESS && !more) {
            // Full response delivered; terminator (OK/ERR) is embedded.
            return true;
        }
        if (length == 0 && status == TEXT_STATUS_IN_PROGRESS) {
            // Peripheral still processing — back off a touch.
            delay(TEXT_TUNNEL_POLL_MS);
        }
    }

    Serial.printf("# [tunnel] addr=%u timeout after %u ms\n",
                  addr, (unsigned)TEXT_TUNNEL_TIMEOUT_MS);
    err("tunnel timeout");
    return false;
}

// Compare the first token of `line` (up to `tokLen` chars) to `target`.
// Used by dispatchAddressed to peek the command name without destroying
// the original line (we need it intact in case we fall through to
// forwardText).
static bool firstTokenIs(const char* line, size_t tokLen, const char* target) {
    return tokLen == strlen(target) && strncmp(line, target, tokLen) == 0;
}

// Dispatch a full command line (cmd + optional args, space-separated) at
// a specific Peripheral. Binary hot-path commands take the fast route
// (one I2C transaction). Everything else goes through the text tunnel.
static void dispatchAddressed(uint8_t addr, char* cmdLine) {
    size_t tokLen = 0;
    while (cmdLine[tokLen] && cmdLine[tokLen] != ' ') tokLen++;
    // Point at args (may be "" if no args — still safe because we keep
    // the full cmdLine around).
    char* args = cmdLine[tokLen] ? cmdLine + tokLen + 1 : cmdLine + tokLen;
    bool hasArgs = (*args != 0);

    // Binary hot path.
    if (firstTokenIs(cmdLine, tokLen, "p"))    { if (fwdSetParameters(addr, args)) ok(); return; }
    if (firstTokenIs(cmdLine, tokLen, "pi"))   { if (fwdSetParameter (addr, args)) ok(); return; }
    if (firstTokenIs(cmdLine, tokLen, "demo")) { if (fwdDemo         (addr, args)) ok(); return; }
    if (firstTokenIs(cmdLine, tokLen, "stop") && !hasArgs) {
        if (fwdStop(addr)) ok();
        return;
    }

    // Everything else — cfg, board, whoami, reboot, help, list,
    // stop <idx>, and any future text command — rides the tunnel.
    // forwardText emits the response (which contains its own OK/ERR
    // terminator) straight to our USB Serial.
    forwardText(addr, cmdLine);
}

// Entry point for lines starting with '@'. `rest` points just past the '@'.
static void handleAddressedLine(char* rest) {
    while (*rest == ' ') rest++;
    char* sp = strchr(rest, ' ');
    if (!sp) { err("usage: @<addr|all> <cmd> [args]"); return; }
    *sp = 0;
    char* addrTok = rest;
    char* cmdLine = sp + 1;

    if (!strcmp(addrTok, "all")) {
        // @all peeks at cmd but destroys cmdLine (it's only used locally).
        char* sp2 = strchr(cmdLine, ' ');
        char* allArgs = (char*)"";
        if (sp2) { *sp2 = 0; allArgs = sp2 + 1; }
        char* allCmd = cmdLine;

        if      (!strcmp(allCmd, "stop")) fanOutStop();
        else if (!strcmp(allCmd, "demo")) fanOutDemo(allArgs);
        else { err("@all supports: stop, demo on|off"); }
        return;
    }

    int addrI = atoi(addrTok);
    if (addrI < 8 || addrI > 119) { err("invalid address"); return; }
    uint8_t addr = (uint8_t)addrI;
    if (rosterFind(addr) < 0) { err("no peripheral at that address"); return; }
    dispatchAddressed(addr, cmdLine);
}

// ------------------------- dispatcher -------------------------

static void handleLine(char* line) {
    aliveAcked = true; // first command received — silence the heartbeat
    while (*line == ' ') line++;
    if (*line == 0) return; // ignore blank lines silently — no OK

    if (line[0] == '@') { handleAddressedLine(line + 1); return; }

    char* sp = strchr(line, ' ');
    char* args = (char*)"";
    if (sp) { *sp = 0; args = sp + 1; }

    if      (!strcmp(line, "whoami"))                         cmdWhoami();
    else if (!strcmp(line, "help") || !strcmp(line, "?"))     cmdHelp();
    else if (!strcmp(line, "reboot"))                         cmdReboot();
    else if (!strcmp(line, "scan"))                           cmdScan();
    else if (!strcmp(line, "list"))                           cmdList();
    else if (!strcmp(line, "forget"))                         cmdForget(args);
    else if (!strcmp(line, "scan_interval"))                  cmdScanInterval(args);
    else if (!strcmp(line, "blink"))                          cmdBlink(args);
    else if (!strcmp(line, "ledoff"))                         cmdLedoff(args);
    else { Serial.printf("# unknown command: %s (try 'help')\n", line); err("unknown command"); }
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

// ------------------------- Arduino entry points -------------------------

void setup() {
    // Zero the roster explicitly (BSS already does, but be plain about it).
    for (uint8_t i = 0; i < MAX_PERIPHERALS; i++) {
        roster[i].inUse = false;
        roster[i].typeCount = 0;
        roster[i].configFetched = false;
        roster[i].lastMissCount = 0;
        roster[i].lastSeenMs = 0;
        roster[i].address = 0;
    }

    // Status LED up first — white while booting, flipped to cyan once
    // setup completes at the end of this function.
    StatusLed::begin();
    StatusLed::setSolid(255, 255, 255);

    Serial.begin(115200);
    delay(2000); // unconditional pause — gives the host time to open the
                 // monitor before boot messages are printed.

    Serial.println();
    Serial.println(F("# =========================="));
    Serial.println(F("# === AO ELECTRONICS CENTRAL ==="));
    Serial.println(F("# =========================="));
    Serial.printf("# fw=%s\n", FW_VERSION);

    // I2C master — pins from build flags (default GPIO 8/9).
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(I2C_FREQ_HZ);
    Serial.printf("# i2c master @ %u Hz (SDA=GPIO%d, SCL=GPIO%d)\n",
                  (unsigned)I2C_FREQ_HZ, I2C_SDA_PIN, I2C_SCL_PIN);

    // One scan at boot so `list` returns something useful immediately.
    doScan();

    // Boot complete → flip LED to role cyan.
    StatusLed::setSolid(0, 255, 255);

    Serial.print(F("> "));
}

void loop() {
    pumpSerial();

    uint32_t now = millis();

    // Heartbeat: every 2 s until the first command is received.
    // Lets you confirm the Central is alive even when boot messages
    // are missed because the monitor was opened after the USB wait expired.
    if (!aliveAcked) {
        static uint32_t lastAliveMs = 0;
        if (now - lastAliveMs >= 2000) {
            Serial.println(F("# alive — type 'help' for commands"));
            Serial.print(F("> "));
            lastAliveMs = now;
        }
    }

    if (scanIntervalMs > 0) {
        if ((now - lastScanMs) >= scanIntervalMs) {
            doScan();
            // Re-print the prompt so the human sees it after a background scan.
            Serial.print(F("> "));
        }
    }
    StatusLed::update(now);

    delay(LOOP_DELAY_MS);
}
