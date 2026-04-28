# Central device — architectural plan

Draft produced by the Plan agent 2026-04-20, decisions signed off by Dave the same day (see "Decisions" below). Going forward this doc is the Central roadmap. When milestones land, their status is updated in `WORKING.md`, not here.

---

## Decisions (2026-04-20)

Answers to the nine open questions in §8, and the refinements they imply.

1. **Address syntax — decimal only.** `@16 cfg`, `@40 stop`. No hex form accepted or printed. Consistency beats "prefer hex for readability" in a small system. `@all` remains.
2. **Address range — 0x08..0x27 accepted (8..39 decimal).** Default `board.json` (address 16) fits. Scans probe 32 addresses.
3. **Text tunnel — adopted.** One `TEXT_WRITE` / `TEXT_READ` pair (opcodes 0x40 / 0x41) carries any non-realtime text command. Realtime stays binary (`p`, `pi`, `stop`, `GET_CONFIG`).
4. **`@all` is fire-and-forget.** Central iterates, sends to each roster member, returns `OK` when all sends have been attempted. Does not wait for device-level ACKs beyond I2C bus-level ACK. Individual NAKs are logged with `# ` but don't fail the command.
5. **Scan on request only, by default.** No periodic scan in v1. Build the scheduler (`scan_interval_ms` configurable via a `scan_interval <ms>` command; 0 = off). Default 0. `scan` forces immediate rescan on demand.
6. **Central has no persistent storage in v1.** Roster rebuilt each boot. Design leaves room for a future `midi_map.json` on Central's LittleFS when M7 lands — plumbed in that milestone, not before.
7. **Central stays on ESP32-S3 indefinitely.** USB-OTG is worth the single-board commitment. Peripherals may move to C6 later; Central doesn't follow.
8. **`@all` with responses — one data line per device.** For fire-and-forget `@all stop` etc. there are no data lines, just `OK`. If future `@all` commands gather responses, format is one line per peripheral prefixed by address: `16: <reply-line>`. Then a single terminator.
9. **Reboot detection — roster carries a `configFetched` flag.** On first ACK from an address that wasn't in the roster (or was forgotten), Central issues `MSG_GET_CONFIG` and sets the flag on success. `list` displays each roster entry's `configFetched` state so the GUI can tell a fresh-seen-but-unread peripheral from a fully-introspected one. A peripheral reboot will miss scans → get forgotten → rediscovered → refetched. Good enough for v1.

### Implications for the plan below

- All worked examples that used `@16` become `@16`.
- Central roster gains `bool configFetched` field.
- Main loop's periodic scan is gated on `scanIntervalMs > 0`; default 0.
- `list` output shows `configFetched` flag per entry, e.g. `16 last_seen=0ms types=[1,2,2] configFetched=yes`.
- New Central command: `scan_interval <ms>` (0 to disable) — runtime setting, not persisted.

---

## 1. Central's responsibilities

**What Central does**

- Bridge USB (Serial + MIDI) ⇄ I2C. Single master on the I2C bus; zero or more Peripherals as slaves.
- Periodically scan the I2C bus, maintain a live roster of Peripherals (address + last-seen + cached type_id summary from `GET_CONFIG_RESPONSE`).
- Accept the same text protocol as Peripheral (with an address prefix — see section 2). Forward each addressed command over I2C as the appropriate binary message.
- Respond directly to Central-scoped commands: `whoami`, `list` (of peripherals), `scan`, `forget`, `broadcast` patterns (e.g. all-stop).
- Act as a **transparent proxy for config/state**: `cfg`/`board` on an addressed peripheral are forwarded; Peripheral answers over I2C in a new chunked-JSON protocol (section 5).
- Translate USB-MIDI Control Change → `SET_PARAMETERS` on an address + element + parameter (mapping table configured later; hooks only for now).

**What Central deliberately does NOT do**

- No LittleFS config, no `config.json` of its own (v1). Roster is in-memory only, rebuilt each boot by scanning.
- No caching of Peripheral config on disk. Every `cfg` call goes to the Peripheral.
- No realtime update loop for local Elements — Central has no Elements.
- No error-translation beyond OK/ERR. It does not attempt to "heal" a flaky peripheral.
- No USB MIDI-to-OSC or OSC input in v1 (future).
- No address assignment (Peripherals set their own `board.json` — Central just discovers them).

## 2. Serial text protocol v2

**Chosen addressing syntax: `@<addr> <cmd> <args...>`** where `<addr>` is decimal (e.g. `16`) or hex (`16`). Both accepted; hex recommended in GUI output for readability.

Rationale for `@<addr>` prefix (vs. inserting address as first positional arg):
- Visually distinctive; a human skimming a log can see what's addressed.
- Keeps the second token a clean command name — the existing Peripheral dispatcher structure is preserved conceptually and could even be reused by peeling the `@<addr>` and invoking the same handler names.
- Central commands (no `@`) don't collide with Peripheral commands.
- Future "group" addresses are natural: `@all`, `@group:foo`.

### Worked examples

| Intent | Command to Central |
|---|---|
| Identify Central | `whoami` → `WHOAMI role=central peripherals=3` then `OK` |
| List known peripherals | `list` → one line per address, then `OK` |
| Scan now (force rescan) | `scan` → `scanned: 16 17 32` then `OK` |
| Remove an unresponsive one from roster | `forget 17` → `OK` |
| Peripheral whoami | `@16 whoami` → `WHOAMI role=peripheral id=16` then `OK` |
| List peripheral elements | `@16 list` |
| Read config | `@16 cfg` → `<json>` then `OK` |
| Write config | `@16 cfg {"...": ...}` → `OK` (forwarded via chunked I2C write — section 5) |
| Read board | `@16 board` |
| Write board | `@16 board {"i2c_address":17}` |
| Reboot a peripheral | `@16 reboot` → `OK` (fire-and-forget; no drop) |
| Stop all on one | `@16 stop` |
| Stop one element | `@16 stop 2` |
| Set all params | `@16 p 1 0.3 0.7` |
| Set one param | `@16 pi 1 0 0.5` |
| Demo on | `@16 demo 1 on` |
| Help | `help` (lists Central + forward syntax) |

**Broadcast forms** (Central-generated fan-out, not I2C general-call):

- `@all stop` — iterate roster, send `MSG_STOP` to each. Fire-and-forget: returns `OK` once all sends have been attempted. Individual bus-level NAKs are logged with `# ` but don't fail the command.
- `@all p 1 0 0` — similarly iterated. (Be wary: element index 1 may mean different things on different peripherals. `@all stop` is the safe case; `@all p ...` is an advanced foot-gun — keep it available but document the semantics.)

### Central-only new commands

- `scan` — force rescan now. Response: `scanned: 16 17 32` then `OK`.
- `scan_interval <ms>` — set periodic scan cadence. `0` disables (default). Runtime-only; not persisted.
- `list` — roster with staleness + `configFetched` flag:
  ```
  3 peripheral(s):
    16 last_seen=0ms types=[1,2,2] configFetched=yes
    17 last_seen=220ms types=[] configFetched=no
    32 last_seen=1500ms types=[4] configFetched=yes (stale)
  OK
  ```
- `forget <addr>` — remove from roster (will rejoin on next successful scan).
- `whoami` → `WHOAMI role=central peripherals=3 fw=v0.1` then `OK`.
- `help` — Central-flavoured help.

### Error cases

- `@34 stop` where 34 is not in roster → `ERR no peripheral at 34`. (Not an I2C attempt — fail fast.)
- I2C NAK on a known-roster peripheral → `ERR bus: nak 16` and mark stale (don't forget yet; next scan will decide).
- I2C timeout → `ERR bus: timeout 16`.
- Malformed `@<addr>` → `ERR invalid address`.
- Unknown Peripheral-side command forwarded: Central can't know; it just sends the line's payload using the relevant I2C message. `cfg`/`board`/`list`/`p`/`pi`/`stop`/`demo` map to defined I2C messages. `help`/`reboot`/`whoami` map to a new I2C "forward-text" mechanism (section 5).

### Framing rules (unchanged from v1)

- One `OK` or `ERR <reason>` line terminates each command.
- Log lines start with `# ` (e.g. `# [scan] found 16`).
- Prompt `> ` after each command.
- No extra chatter.

## 3. Central firmware design

### Main loop

```
setup():
  Serial.begin; wait for CDC
  Wire.begin() as master @ 100 kHz (conservative; bump to 400k later if stable)
  doScan()
  lastScanMs = millis()
  Serial.print("> ")

loop():
  pumpSerial()                                     // text protocol in/out
  if (now - lastScanMs > SCAN_INTERVAL_MS) {       // 2000 ms default
    doScan(); lastScanMs = now;
  }
  delay(LOOP_DELAY_MS)    // 5 ms — keep USB CDC responsive
```

No blocking in the command path; each command must complete in <200 ms or return `ERR bus: timeout`.

### Data structures

```cpp
static const uint8_t MAX_PERIPHERALS = 32;

struct PeripheralRec {
    uint8_t  address;                      // 7-bit I2C
    uint32_t lastSeenMs;
    uint8_t  typeCount;
    uint8_t  typeIds[MAX_ELEMENTS_PER_BOARD]; // cache from last GET_CONFIG_RESPONSE
    bool     inUse;
};

static PeripheralRec roster[MAX_PERIPHERALS];
static uint8_t       rosterCount = 0;
```

Roster is flat; linear search by address (N≤32, trivial cost). No dynamic allocation.

### Enumeration strategy

- **Scan range: 0x08..0x27 (32 addresses).** 7-bit I2C reserves 0x00..0x07 and 0x78..0x7F; the standard "safe" range is 0x08..0x77. Dave's ask — keep scans fast — so we constrain Peripherals by convention to 0x08..0x27 (32 slots, one or two orders of magnitude more than we need). This keeps a full scan to 32 probes.
- **Probe method: 0-length write + ACK test.** `Wire.beginTransmission(addr); Wire.endTransmission();` returns 0 on ACK. Standard, reliable.
- **Scan timing:** on boot (before first `> `), then every 2 s in background. `scan` command forces an immediate rescan.
- **Transient failures:** a miss doesn't forget. Three consecutive misses → mark stale (`lastSeenMs` old). Six consecutive misses → remove from roster. This buys time around Peripheral reboots (e.g. after `@16 reboot`).
- **New peripheral found:** send `MSG_GET_CONFIG`, cache the `type_ids[]` into the roster entry. If Peripheral doesn't respond, keep it in roster with `typeCount=0` — it's present but not introspected yet.

### I2C master implementation — reuse and gaps

Existing messages are fine for realtime control:
- `MSG_STOP (0x32)` — 1-byte send.
- `MSG_SET_PARAMETERS (0x31)` — Central builds the byte frame from `p`/`pi` args.
- `MSG_GET_CONFIG (0x30)` + `GET_CONFIG_RESPONSE` — for the periodic "did they change their element set?" refresh.

Gaps (proposal in section 5):
- `cfg` / `board` read full JSON — need a **chunked request/response** (or forwarded text).
- `cfg <json>` / `board <json>` write — need a **chunked write**.
- `reboot`, `whoami`, `demo`, `stop <idx>`, `list` — either new dedicated I2C messages or a **"forward text command"** envelope.

Recommendation (section 5): add one generic text-tunnel message pair, avoid proliferating I2C opcodes. The realtime path (`p`, `pi`, `stop`, broadcast `STOP`) stays binary for speed; everything else tunnels text.

### Boot flow

1. USB CDC up.
2. Wire master up.
3. Scan immediately. Log each hit as `# [scan] 16 types=[1,2,2]`. Empty scan is fine — log `# [scan] no peripherals found` and continue.
4. Emit `> `.
5. Enter main loop.

### Error-handling policy

- I2C NAK on a roster peripheral → `ERR bus: nak 0x<addr>`. Mark stale. Do not retry automatically for realtime commands (caller should decide).
- I2C bus lock-up (all writes fail for >3 seconds) → call a `recoverBus()` that clocks SCL 9 times manually. Log, continue. If still stuck, keep logging but don't deadlock the loop.
- Response size mismatch (e.g. `GET_CONFIG_RESPONSE` declares 5 types but we only read 3 bytes) → log, discard response, don't update cache.
- Roster full → log and skip new discoveries. (32 is generous.)

## 4. GUI adaptations

### Role detection flow

```
connect → whoami
if role == 'peripheral':  existing flow (one card)
if role == 'central':
    list = await protocol.listPeripherals()   // parses `list` response
    for each peripheral addr: fetch board + cfg via `@<addr> board` / `@<addr> cfg`
    render one PeripheralCard per addr, using same builder
```

### New `Protocol` helpers

Preferred: make the existing helpers accept an optional `addr`:

```js
async readConfig(addr = null) {
    const prefix = addr == null ? '' : `@${formatAddr(addr)} `;
    const lines = await this.send(`${prefix}cfg`);
    return JSON.parse(lines.join('\n'));
}
```

Fewer symbols, and `app.js` threads the peripheral's address through uniformly. When connected to a Peripheral directly, `addr` is `null` and no prefix is emitted — bit-identical to today.

Plus Central-only helpers:
- `listPeripherals()` — parses `list` output into `[{addr, typeIds, stale}]`.
- `scan()` / `forget(addr)`.

### Reuse of per-Peripheral UI

- `renderPeripheralCard(dev)` takes `dev` already. Add `dev.address` (null when direct). Everywhere a command is sent, thread `dev.address` through to the protocol helper.
- `sendParam(rec, i, value)` → `protocol.sendPi(dev.address, rec.dataEntry.index, i, value)`.
- Board Stop / Demo / per-element demo/stop/config / add/remove / save elements / save board — all same shape, just prefix-aware.
- `rebuildSummaryRows`, slider sync logic — unchanged.

### "Multiple cards" layout

Already exists. When Central returns N peripherals, simply render N cards into `#devices`.

### Top-bar additions when connected to Central

- "Rescan" button → `protocol.scan()` then re-render.
- "Stop All Peripherals" button → `protocol.send('@all stop')`.
- Add a "Peripherals" badge: `3 online, 0 stale`.

### UX for Peripheral offline mid-session

- Each PeripheralCard polls lightly (e.g. on user command failure with `ERR bus: nak|timeout`): mark the card `.stale`, dim it, show a "Last seen 12s ago — Retry" button.
- Stop all outstanding sliders (abort in-flight `pi` sends cleanly).
- When a `scan` result later omits that address for >6 s, remove the card and log.
- User can click "Rescan" to force recovery.

### UX for rescan / re-enumerate

- Explicit button in the Central top bar.
- When a card is removed by rescan, any open config editor in it is discarded (with toast warning; don't block).
- When a card is added (new peripheral appeared), slide it in at the end.

## 5. Remaining I2C protocol gaps

**Problem:** The GUI needs the Peripheral's full config JSON (so it can render element cards with pins and defaults) via the Central. Today's I2C `GET_CONFIG_RESPONSE` only gives type_id bytes — fine for quick roster summaries, insufficient for the GUI.

### Options considered

1. **New I2C messages per need** (`GET_CONFIG_JSON`, `GET_BOARD_JSON`, `SET_CONFIG_JSON`, etc.) — proliferates opcodes; each needs its own chunking logic.
2. **Per-element detail endpoint** (`GET_ELEMENT <idx>` → struct) — still needs a schema-aware serializer, and new opcodes for board info, reboot, demo, stop-one.
3. **Text tunnel over I2C** — one new request/response pair that carries a text line in + text lines out (with OK/ERR). All non-realtime commands reuse this. The Peripheral's existing text dispatcher runs them.

### Decision: option 3 — add a generic text tunnel

Trade-off accepted: a little extra I2C overhead for non-realtime commands (acceptable — `cfg` is user-triggered, not 60 Hz), in exchange for **one** new mechanism that covers `cfg`/`board` read, `cfg`/`board` write, `whoami`, `list`, `reboot`, `demo`, `stop <idx>` — and automatically every future text command with zero I2C-side changes.

Keep `MSG_SET_PARAMETERS`, `MSG_STOP`, `MSG_GET_CONFIG` as binary (hot path / roster).

#### New I2C messages (proposed)

```
0x40 TEXT_WRITE
    offset: uint16 LE      // starts at 0; ≥1 for continuation
    length: uint8          // payload bytes in this chunk (≤ 28, leaves headroom in 32-byte Wire buffer)
    more:   uint8          // 1 = more chunks coming, 0 = last chunk
    data:   length bytes

0x41 TEXT_READ
    offset: uint16 LE
    maxlen: uint8          // caller-requested chunk size (e.g. 28)

TEXT_READ response:
    length: uint8          // bytes returned this read
    more:   uint8          // 1 = more available, 0 = end
    status: uint8          // 0=in-progress, 1=OK (complete), 2=ERR (complete)
    data:   length bytes   // UTF-8 text; newlines preserved
```

Peripheral side (new state machine):
- A single-slot `TextReq`/`TextResp` buffer (say 4 KB — matches CLI_BUF_SIZE).
- `TEXT_WRITE` accumulates into the request buffer. When `more=0`, the Peripheral hands the assembled line to the **existing text dispatcher** (re-using `handleLine`), capturing all output to the response buffer with an OK/ERR terminator.
- `TEXT_READ` drains the response buffer; `status` tells the Central when reading is complete.
- Only one text request in flight per Peripheral; Central serialises.

Central side:
- `forwardText(addr, "cfg")` → writes chunks with `TEXT_WRITE` → polls `TEXT_READ` until `status != in-progress` → returns `{ok, lines[]}`.
- Map directly into the USB Serial `OK`/`ERR` + preceding data lines. Zero translation.

This is a deliberate "cheap Unix pipe over I2C" — not elegant, but drastically shrinks the design surface. Revisit if/when the Central sprouts proper WiFi/Matter transport (in that world we'd probably drop I2C entirely and this code goes away).

#### Small refactor on Peripheral

- Extract `handleLine` so it can write its output to a `Print*` target rather than directly to `Serial`. Use `Stream` or a tiny `Print` subclass that writes into a buffer. On USB path the target is `Serial`; on the I2C tunnel path it's the response buffer. This touches `peripheral/main.cpp` only.
- Adds a small ISR/interrupt discipline: Wire events can fire off-loop on ESP32-S3; assemble them into the request buffer but only process a completed line from the main loop (`pumpI2C()` analogous to `pumpSerial()`).

## 6. Hardware

- **Central MCU:** **stay on Waveshare ESP32-S3-Zero** for now, same as Peripheral. Reasons: keeps `platformio.ini` symmetrical; S3 has USB-OTG which is required for TinyUSB composite devices (MIDI + CDC). C6 is ok for Peripherals but its USB is USB-Serial/JTAG only — **TinyUSB MIDI on C6 is not currently supported**. Keep Central on S3 unless and until a C6 with USB-OTG / external USB transceiver appears.
- **USB MIDI approach:** `Adafruit_TinyUSB_Arduino` library (v1.x+ supports ESP32-S3 via the Arduino core's TinyUSB integration). Build flag: `-DUSE_TINYUSB=1` **instead of** `-DARDUINO_USB_CDC_ON_BOOT=1` when Adafruit TinyUSB takes over the USB stack. It provides a composite CDC+MIDI descriptor — you get `Serial` (CDC) and `MIDIUSB` together. Known gotchas:
  - On arduino-esp32 v3.x, Adafruit_TinyUSB integrates cleanly; v2.x needs the right env switches. Current project pins `espressif32 ^6.9.0` which is arduino-esp32 v3.x territory (WORKING.md already chose this for `ledcAttach`, so good).
  - Descriptor setup code must run in `setup()` before `Serial.begin()` — a few-line boilerplate.
- **I2C wiring:** pull-ups (4.7 kΩ) on SDA/SCL. Central's I2C pins: any free GPIO; recommend pins that survive boot-strapping cleanly (on S3, GPIO 8/9 are the `Wire` defaults and fine).
- **Power:** USB host supplies Central; Peripherals may need their own supply when driving motors — out of scope for firmware.
- **Pin budget for Central:** only I2C (2 pins) and USB (dedicated). The S3-Zero has plenty to spare if we later want an OLED, status LEDs, etc.

### `platformio.ini` changes required

Add Central-specific `build_flags` and `lib_deps` under `[env:central]`:

```
[env:central]
build_src_filter = +<central/>
build_flags =
    ${env.build_flags}
    -DUSE_TINYUSB=1
lib_deps =
    ${env.lib_deps}
    adafruit/Adafruit TinyUSB Library@^3.2.0
```

When USB MIDI comes online we may need to remove `-DARDUINO_USB_CDC_ON_BOOT=1` from `[env]` and move it into `[env:peripheral]` only — TinyUSB composite handles CDC itself for Central.

## 7. Build order / milestones

Each milestone compiles, flashes, and is testable over USB Serial.

**M0 — Central compiles and identifies itself.**
- `src/central/main.cpp`: Serial up, emit banner, respond to `whoami` → `WHOAMI role=central peripherals=0 fw=v0.1`.
- `help` stub.
- Test: attach, type `whoami`, verify.

**M1 — I2C scan + roster + `list` / `scan` / `forget`.**
- Wire master; `roster[]`; `doScan()`; staleness.
- Commands: `list`, `scan`, `forget`.
- Test: power a Peripheral (address 16), `scan`, `list`. Unpower → 6 s later disappears from list. Repower → reappears.

**M2 — Forward binary-hot-path commands (`stop`, `stop <idx>`, `p`, `pi`).**
- `@<addr>` parser.
- `@16 stop` → `MSG_STOP`.
- `@16 p 1 0.3` → `MSG_SET_PARAMETERS`.
- `@16 pi 1 0 0.5` → build one-param `MSG_SET_PARAMETERS`.
- `@all stop` fan-out.
- Test: with PWM_MOTOR peripheral, make motor spin via `@16 pi 1 0 0.5`, stop via `@all stop`. Good first "end-to-end through Central" feel-check.

**M3 — Text tunnel (Peripheral side).**
- Refactor `handleLine` to accept a `Print*` target.
- Add `pumpI2C()` and Wire onReceive/onRequest for opcodes `0x40` TEXT_WRITE / `0x41` TEXT_READ.
- Test locally first by issuing identical text via USB serial vs. text tunnel (using a PC-side script that talks I2C directly — or via M4).

**M4 — Text tunnel (Central side) + forwarded non-realtime commands.**
- `forwardText(addr, "cfg")`, etc.
- Wire up: `@16 cfg`, `@16 cfg <json>`, `@16 board`, `@16 board <json>`, `@16 whoami`, `@16 list`, `@16 demo 1 on`, `@16 reboot`, `@16 help`.
- Test: read + write a config through Central. Reboot through Central. Everything the GUI currently does direct-to-Peripheral now works through Central.

**M5 — GUI: Central role.**
- Add `addr` param to Protocol helpers.
- On `whoami role=central`, enumerate peripherals, render N cards.
- Rescan button, Stop All button, staleness indicator.
- Test: connect GUI to Central; drive motor(s) on two peripherals.

**M6 — USB MIDI skeleton.**
- Adafruit_TinyUSB CDC+MIDI composite descriptor.
- Central appears as both a serial device and a MIDI device on the host.
- Log incoming MIDI CC to Serial; no mapping yet.
- Test: Ableton/Pd sees it as a MIDI port; CC messages log on Serial.

**M7 — MIDI CC → SET_PARAMETERS mapping.**
- Config mapping (JSON): `{ channel, cc, addr, element_index, parameter_id }` tuples.
- Initially hard-coded or loaded from LittleFS `midi_map.json`; `midi <json>` and `midi` commands to read/write.
- Test: slider in DAW moves motor.

**M8 — Polish / robustness.**
- Bus recovery (9-pulse SCL on wedge).
- Retry-once policy on NAK for non-realtime text tunnel commands.
- `stop` broadcast via I2C general call (address 0x00) as an optional fast path — evaluate after hardware testing.

M0–M4 are the heart of the plan. M5 is the GUI payoff. M6+ can slide.

## 8. Open questions for Dave

1. **Address syntax.** `@16 cfg` vs `@16 cfg` vs just decimal — I've picked "accept both, print hex" for readability. OK?
2. **Address range for Peripherals.** I'm proposing **0x08..0x27** (32 slots). Fast scans, room to grow. Confirm? `data/board.json` in the repo already uses `16`, which fits.
3. **Text tunnel vs. per-command I2C opcodes.** Text tunnel is the cleaner choice for maintenance, but adds latency (multiple I2C transactions per `cfg`). For a studio tool this is invisible. Sign-off?
4. **Where does `@all` iterate?** Central-side fan-out (my plan) vs. I2C general call (address 0x00 hardware broadcast). General call is one wire transaction but only Peripherals that opt in will hear it, and there's no per-device ACK. I suggest Central-side fan-out for correctness; general-call as an optional fast `@all stop` later.
5. **Scan cadence.** 2 s default. OK, or too chatty on the bus during music playback? Alternative: scan only on explicit `scan` + passive detection (a peripheral that ACKs a real command stays "alive" without dedicated probes). I'd favour passive-plus-on-demand once the system is mature; 2 s polling is a fine development-phase default.
6. **Should Central store anything on LittleFS?** My plan says no for v1 (roster rebuilt on boot). But a `midi_map.json` comes in M7 — happy with that single file on Central?
7. **USB MIDI on S3-Zero.** Are you OK with Central staying on S3 indefinitely, or do you want me to design with an eye to eventually moving MIDI off the C6-incompatible path (e.g. MIDI-over-UART to a dedicated USB-MIDI dongle)?
8. **Error semantics on `@all`.** I'm proposing `ERR partial: 0x11 timeout, 0x20 nak` when some succeed and some fail — but the OK/ERR framing is binary. Alternative: always `OK` with a data line per peripheral reporting its status. Which is more useful for the GUI?
9. **Does a Peripheral reboot invalidate its cached `typeIds[]`?** My plan: yes — on first post-reboot scan hit, refetch `MSG_GET_CONFIG`. Any concern about the in-between window where the GUI thinks the config is what it was pre-reboot?
