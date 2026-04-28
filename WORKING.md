# I2CElectronics — Working Notes

This file is where Claude keeps ongoing thinking, plans, and open questions for the I2CElectronics subproject. Dave inspects it by hand. `OVERVIEW.md` is the hand-authored brief and evolves slowly; this file can churn freely.

---

## GUI M5 polish — queue fix + deferred config read (2026-04-21)

**Bug fix: protocol queue poisoning.** When `@<addr> reboot` timed out (peripheral restarts before replying), its rejection propagated down the `_queue.then(...)` chain, so every *subsequent* queued command inherited the rejection and was never actually sent — surfacing as a phantom `ERR bus` (or whatever the original error was) on every rescan. Symptom: dropping+reconnecting the serial fixed it (fresh Protocol, fresh queue). Fix in `gui/protocol.js`: use `.then(onFulfilled, onRejected)` with the same runner on both paths, and store the tail as `.catch(() => {})` so the queue is never in a rejected state for the next caller.

**Deferred config read for Central mode.** Previously, connecting to a Central triggered a full `board` + `cfg` tunnel read for *every* peripheral — slow for N>1, and wasted for peripherals the user never edits. Now:
- Central enumeration only uses the `typeIds` from the `list` output to build synthetic element entries (index + type, empty config).
- Sliders / demo / stop all work immediately from synthetic data.
- Full `board` + `cfg` fetch is deferred until the user opens a Config UI (`ensureConfigLoaded(dev)`), or clicks Reload.
- First click on **Config** (peripheral-level or per-element) when unloaded triggers the fetch and re-renders the card expanded.
- First click on **Save board** or **Save elements** when unloaded triggers the fetch and logs "review values and click Save again to write" — prevents accidental overwrite with synthetic blanks.
- **Reload** button now forces a re-fetch (`dev.configLoaded = false` then `ensureConfigLoaded`).
- **Rescan** preserves already-loaded devices so reloaded state survives a rescan.
- Header shows "config not loaded — click Config to edit" when `dev.configLoaded === false`.

**Direct peripheral mode unchanged** — still eager-loads board + cfg at connect (one fast local call).

**Add-element dialog — defensive fix (2026-04-21):** Dave reported the Add button did nothing after opening the dialog. Most likely `wa-select.value` was empty at click time (attribute-set `.value` not reflecting because the options hadn't fully upgraded). Fixes in `openAddElementDialog`:
- First option is marked with the `selected` attribute (attribute-based, more reliable than setting `.value` in rAF).
- The Add handler now falls back through three sources: `select.value` → `wa-option[selected]` attribute → first `wa-option` → first schema entry. No path should leave `typeName` empty.
- Failure log includes the raw `select.value` so a future regression is easy to diagnose.
- Added Enter-key trigger as a second path in case Web Awesome's wa-button click isn't firing under some timing.

**PWM_MOTOR INVERT + boolean type (2026-04-26):** Added an `INVERT` boolean config field to PWM_MOTOR for active-low motor drivers (e.g. high-side P-channel MOSFET) where the motor sees full power when the pin is pulled low. Implementation: a single `if (invert) duty = 255 - duty;` at the bottom of `writePwmFromCurrent()`. Off by default. Round-trips through save/reboot/cfg-read intact.

While at it, added `boolean` type support across the schema/UI stack so future Elements can use checkboxes instead of typing 0/1 ints. Specifics:
- `gui/ui-schema.js`: new `booleanCheckbox` builder — labelled wrap with a native `<input type="checkbox">` and an "on"/"off" word next to it. Returns a real boolean from `getValue()`.
- `gui/styles.css`: small block for `.config-field-bool-row` matching the other field wraps.
- `CONFIG_TYPES.md`: boolean moved out of "future" and into the canonical type table.
- `shared/control_schema.json`: PWM_MOTOR's INVERT field uses `"type": "boolean"`.
- ArduinoJson handles `cfg["INVERT"] | false` cleanly — accepts `true`/`false`/`1`/`0` in JSON and falls back to `false` if the field is absent (so old configs without INVERT still load).

**SERVO_ARM Element implemented (2026-04-23):** Two-servo arm aiming a laser at a wall. ANGLE parameter sets the dot position on the wall (angle from straight-out); OFFSET sets the base servo angle directly; the upper servo is computed from the geometry (`a2 = atan2(T.y − U.y, T.x − U.x) + a1 − 90°`) to keep the dot on target as the arm swings. Kinematics verified beforehand in an interactive `gui/servo_arm_sim.html` simulation — user confirmed the math matches physical reality.

Schema (`shared/control_schema.json`) for SERVO_ARM expanded with four new fields: `FOCUS_DISTANCE_MM` (d1, base→wall), `ARM_LENGTH_MM` (d2), `MIN_BEAM_ANGLE_DEG` / `MAX_BEAM_ANGLE_DEG` (beam-angle extremes on the wall, mapped from ANGLE parameter). Existing `MIN/MAX_BASE_POSITION` maps OFFSET → base servo angle; `MIN/MAX_UPPER_POSITION` is a mechanical safety clamp for the computed upper servo angle.

New shared helper `lib/AOElectronics/ServoAllocator.{h,cpp}` — a simple namespace with `allocate()` returning the next free LEDC channel from the high end (7 down). Both SERVO_MOTOR and SERVO_ARM now use it, so servo channels and analogWrite (starting from 0) share the 8-channel pool without collision. SERVO_MOTOR's old file-local counter removed.

Demo mode: independent triangle waves on ANGLE (6 s period) and OFFSET (10 s period). Different periods mean the pattern doesn't loop obviously — suitable for an art installation where you want slow, non-repetitive motion. Phase accumulators reset on `onDemoStart` so demos start deterministically.

Verified on hardware via serial: `cfg` save + reboot + `list` shows the element; `pi 1 0 0.5 / pi 1 1 0.5`, extreme values, and `demo 1 on/off` all respond OK. Actual motion with servos attached still needs hardware-in-the-loop verification — the LEDC pulse math mirrors the working SERVO_MOTOR implementation so should be correct.

**StatusLed quality-of-life modes (2026-04-23):** Turned the plain pixel into a small state machine (`lib/AOElectronics/StatusLed.{h,cpp}`). Modes: Solid (base role colour), Pulse (temporary override colour with timed revert), Blink (base colour flashing 2 Hz with timed revert), Dark (off with timed revert). `update(now)` called from each env's `loop()` drives transitions.

Uses:
- Boot sequence: white at setup start → role colour (green/cyan) once setup completes.
- cfg / board save success on peripheral: 300 ms white Pulse as visible confirmation.
- `blink [seconds]` text command (default 5 s) — flashes the role colour so a human can pick out the connected board.
- `ledoff [seconds]` text command (default 10 s) — turns LED off for the duration; fan this out to every board and a stuck/crashed one is the one that stays lit.

GUI additions:
- Per-card **Blink** button (sends `blink 5` via direct or `@<addr> blink 5` via tunnel).
- Top-bar **Identify all** button — in Central mode iterates `ledoff 10` through every roster peripheral + the Central itself. In direct mode targets the single device. Fan-out is sequential through the tunnel (~50 ms per peripheral), acceptable given the 10 s off window.

All commands pass through the existing text-tunnel path when addressed, so no new binary I2C opcode was needed. LED hardware writes are throttled: `update()` only calls `pixel.show()` when the displayed colour actually changes.

**USB CDC RX buffer — the real "add 2nd element" bug (2026-04-23):** Dave's GUI log showed the 3-element `cfg <json>` payload (~470 bytes) was sent correctly; the peripheral silently consumed it and replied `ERR invalid json` 10+ seconds later as an "unsolicited" line. Cause: arduino-esp32 v2.x's native USB CDC RX ring buffer defaults to **256 bytes** — smaller than the payload. Depending on driver version, excess bytes either stall flow control (explaining the huge delay) or get dropped outright (explaining the invalid-JSON error). Fix in `src/peripheral/main.cpp`: call `Serial.setRxBufferSize(CLI_BUF_SIZE)` **before** `Serial.begin()` so the 4 KB ring buffer is allocated from the start. Also added a `# [cmd] len=NNN "…"` diagnostic log inside `handleLine` so any future truncation is visible. GUI-side `writeConfig` timeout bumped from 10 s to 20 s for additional headroom.

This was masquerading as a GUI bug ("2nd element doesn't appear") — the GUI was doing the right thing; the save silently failed on the peripheral, and after a reboot the config was unchanged, so the user only saw the N-1 elements they started with.

**ensureConfigLoaded — re-render race fix (2026-04-23):** `renderPeripheralCard` mutates `dev._cardEl` to the new card *inside* the function (before returning). The caller then did `dev._cardEl.parentNode.replaceChild(newCard, dev._cardEl)` — but by then `dev._cardEl` was already the new (orphan) card, so `replaceChild` silently no-oped and the old card stayed. Fix: capture `oldCard = dev._cardEl` before calling `renderPeripheralCard`, replace against the captured ref. Affected Central mode and the Reload button in any mode.

**Add-element dialog — diagnostic instrumentation + robustness (2026-04-23):** Suspected cause of the "2nd element silently missing" report: the close handler appended the new card to `section.querySelector('.elements-list')` where `section` was captured at open-time — if the peripheral card was rebuilt between clicking `+ Add` and closing the dialog, `section` was a detached DOM subtree and the new card ended up in a ghost tree (rendered into memory, never reparented into the document). Fix:
- Prefer `dev._cardEl?.querySelector('.elements-list')` (always live); fall back to the captured `section`.
- Explicitly check `list.isConnected` and bail with an informative log if it's not.
- Wrap the whole close handler in `try { … } finally { dlg.remove() }` so the dialog always cleans up, even on early-return paths (prior code leaked dialog nodes when typeEntry/list/cardEl checks failed).
- Added `console.log('[add-element] …')` breadcrumbs at every step so DevTools can show exactly where a future regression lands.

**Pin dropdown — expose strapping pins with warning labels (2026-04-23):** Dave pointed out GPIO 18 is broken out on the Waveshare schematic and is safe (non-strapping, non-USB, non-I2C). Added to `usable`. Also split the previous `reserved` map into `caution` (GPIO 0, 3 — strapping, pickable but annotated `"(strapping — must be HIGH at boot, avoid motor drivers)"`) and `reserved` (I2C 8/9, USB 19/20 — hidden from dropdown unless legacy config already uses them). Dropdown order: usable → caution → any prepended non-standard legacy option.

**Pin-picker UX + conflict detection (2026-04-23):** The "always uses pin 7" bug wasn't firmware — the user had set PWM_PIN=8 and PWM_PIN=9, which are the **I2C slave pins** (SDA/SCL). The PWM outputs were dormant because the I2C slave init (which runs after element init) re-muxes those pins. Confirmed by sending `cfg` + `list` directly over the serial port: both elements were loaded, just with pins that the I2C slave was contesting.

**Fixes:**
- `gui/ui-schema.js`: the `pin_id` control is now a native `<select>` drawn from a `BOARD_PINS.usable` list (currently `[1,2,4,5,6,7,10]` for the Waveshare ESP32-S3-Zero — excludes strapping pins 0/3 and I2C 8/9). When a legacy config has a reserved pin, it's offered as a prepended `"GPIO N (I2C SDA)"` style option so the user can see and change it, rather than silently being overwritten.
- `gui/app.js` added `checkPinConflicts(dev)` — scans every `pin_id` field across a peripheral's elements, flags duplicates with a red border + "pin used by another element" hint. Runs on first render, on any pin change, and on add/remove.
- `CONFIG_TYPES.md` updated to reflect the new dropdown behaviour.

The `BOARD_PINS` map will need updating when the user moves to XIAO ESP32-C6; ideally longer-term this gets sourced from firmware (e.g. a `pins` query) so the GUI stays right across hardware variants.

## Debug infrastructure (2026-04-23)

Got direct serial access working for debug sessions:
- `~/.platformio/penv/bin/pio` accessible without allowlist changes.
- Project-local `.tmp/` directory (gitignored) holds backgrounded serial logs. Start/stop pattern: `stty -f /dev/cu.usbmodem101 115200 cs8 -parenb -cstopb raw -echo; cat /dev/cu.usbmodem101 > .tmp/periph.log & echo $! > .tmp/periph.pid`. Send commands with `printf "<cmd>\n" > /dev/cu.usbmodem101`. Kill with `kill $(cat .tmp/periph.pid)`. Only one process holds the port at a time so user closes the GUI for the duration.

**PWM_MOTOR frequency — switched to direct LEDC (2026-04-22):** Dave tried driving a motor and it wouldn't respond. `analogWrite` default on arduino-esp32 v2.x is ~1 kHz, well below the 15-25 kHz most H-bridge drivers want (and audibly whines). Rewrote PWM_MOTOR to drive LEDC directly (same pattern as SERVO_MOTOR): `ledcSetup(channel, freq, 8)` + `ledcAttachPin` + `ledcWrite`. Frequency now configurable via a new `PWM_FREQ` field in `control_schema.json` (default 20000). Channel allocator counts up from 0; SERVO_MOTOR counts down from 7, so a board still gets up to 8 LEDC Elements total. Elements pre-dating the schema change will take the default 20 kHz automatically (ArduinoJson `|` defaulting).

**Add-element dialog — scrapped and rewritten (2026-04-22):** Two rounds of workarounds piled on top of each other (overlay prevention, gated-close flag, value fallbacks, Enter-key handler) and the dialog still misbehaved with `wa-select` — Dave reported the Cancel/Add buttons had disappeared entirely. Pulled the whole approach and switched to a **plain native `<dialog>` with plain `<select>` and `<button>`**. Zero Web Awesome components in the dialog. Uses `method="dialog"` form submit for Add, a type-button click for Cancel, close event dispatches the result. Styled with a new `.simple-dialog` CSS block. Lesson logged: reach for native HTML first when an interactive flow gets shaky, swap in component-library visuals only if the plain version is truly inadequate.

## GUI M5 — Central role + multi-peripheral rendering (2026-04-20)

GUI now adapts to whichever role the connected device reports on `whoami`. When talking to a Central, the GUI enumerates peripherals via `list`, fetches `board` and `cfg` for each via the tunnel, and renders a card per Peripheral. Existing card-building, sliders, demo toggles, save/reboot flows all now thread `dev.address` through to `Protocol`, which prefixes `@<addr>` when set.

**Protocol changes** (`gui/protocol.js`):
- Every helper (`whoami`, `readConfig`, `writeConfig`, `readBoard`, `writeBoard`, `reboot`) takes an optional `addr`. Null / undefined = direct; number = prefixed with `@<addr>`.
- `listPeripherals()` — parses Central's `list` output into `[{address, typeIds, configFetched, stale, lastSeenMs}]`.
- `scan()` — triggers a Central rescan.
- `reboot(addr)` silently swallows the expected tunnel timeout when rebooting a Peripheral through Central (peripheral resets before emitting OK).

**App changes** (`gui/app.js`):
- State now `state.devices[]` + `state.role`. Always an array, even for direct peripheral (single entry with `address=null`).
- `queryDevices()` branches on role: peripheral → one device; central → enumerate + build each.
- `buildDevice(addr, listInfo)` helper — fetches board + config, tolerates partial failure.
- `rescan()` — triggers `scan`, re-enumerates, fully rebuilds devices and cards.
- `devCmd(dev, cmd)` — uniform way to prepend `@<addr>` for command-string sends (demo, stop, pi, etc.).
- Top-bar **Rescan** button appears only when connected to Central (hidden and disabled otherwise).
- **Stop All** dispatches role-aware: `@all stop` when Central, `stop` when direct peripheral.
- Connection status badge now reports `connected — peripheral` or `connected — central (N peripherals)`.
- Empty-central path: card area says "Central connected — no peripherals found. Wire up a Peripheral and click Rescan."
- Reload / Reboot buttons are per-device (were before, now actually wire through the right address).
- When rebooting a Peripheral via Central: auto-rescan after 1.5 s to recover state.

**Index / styles:** just a new `<wa-button id="rescan-btn">` in the topbar. CSS unchanged.

**Tested path (documented, pending hardware verification):**
- Connect to peripheral directly → one card, same as before M5.
- Connect to Central → N cards (one per peripheral). All sliders / demo toggles / saves go through the right address.
- Rescan after adding/removing a peripheral physically → cards reflect new roster.

**Not in M5 (future rounds):**
- Per-card offline detection mid-session (if a peripheral drops between polls, card stays stale until user clicks Rescan).
- Drag-to-reorder elements.
- Persistent GUI state across reconnects.

## Central M3 + M4 — text tunnel (2026-04-20)

Adds an I2C transport for **any** Peripheral text command, not just the binary hot-path ones. Now `@<addr> cfg`, `@<addr> whoami`, `@<addr> reboot`, etc. all work through the Central.

**Two new binary I2C messages** in `Protocol.h` + `shared/i2c_messages.json`:
- `MSG_TEXT_WRITE` (0x40) — chunked request: `offset:u16 LE`, `length:u8`, `more:u8`, `data`.
- `MSG_TEXT_READ` (0x41) — chunked response poll: `offset:u16 LE`, `maxlen:u8`. Peripheral replies with `length:u8`, `more:u8`, `status:u8` (0=in-progress / 1=OK / 2=ERR), then data.

**Peripheral side (M3)** in `src/peripheral/main.cpp`:
- New `BufferPrint` (Arduino `Print` subclass) writes into a 4 KB response buffer.
- File-scope `Print* g_out = &Serial` — all command handlers now write via `g_out->` rather than `Serial.` directly. Async / boot / log output stays on `Serial`.
- Text-tunnel state: request buffer, response buffer, status, flags (`cmdEmittedOk`, `cmdEmittedErr`). All volatile — `onI2CReceive` (Wire task) is the writer for the request side, main loop's `pumpI2C()` is the writer for the response side, `onI2CRequest` reads both.
- `onI2CReceive` handles `MSG_TEXT_WRITE` chunks (assembling into the request buffer) and stages `MSG_TEXT_READ` requests for `onI2CRequest`.
- `pumpI2C()` in the main loop: when a request is ready, swap `g_out` to the response buffer, run `handleLine`, flip status based on which flag was set. Empty lines synthesise `ERR empty command` so every request completes.
- Size ordering: response length published before status flips off `IN_PROGRESS` — prevents a reader from seeing a non-IN_PROGRESS status with stale length.

**Central side (M4)** in `src/central/main.cpp`:
- `forwardText(addr, line)` sends the command in `TEXT_CHUNK_MAX=64`-byte chunks, polls `TEXT_READ` with increasing offset, streams each response chunk straight to the Central's USB Serial, exits when `status != IN_PROGRESS && !more`.
- 5-second tunnel timeout, 10 ms poll backoff when status is `IN_PROGRESS` and data is empty.
- `dispatchAddressed` refactored to take the full `cmdLine` (not pre-split): peeks the first token with a `firstTokenIs` helper. Binary hot-path commands (`p`, `pi`, `demo`, `stop` with no args) take the fast path; everything else (including `stop <idx>`, `cfg`, `board`, `whoami`, `reboot`, `help`, `list`) falls through to `forwardText`.
- Response terminator (`OK` / `ERR`) is embedded in the peripheral's output — Central doesn't add its own after a successful tunnel round trip. On bus error / timeout, `err()` is emitted.

**Testable now on two boards:**
- `@16 whoami` — peripheral's identity through Central.
- `@16 cfg` — dumps config.json.
- `@16 cfg {"...":...}` — writes new config.json to the peripheral.
- `@16 board`, `@16 board {"i2c_address":17}` — read/write board settings.
- `@16 list` — peripheral's own element list.
- `@16 demo 1 on`, `@16 pi 1 0 0.5` — still binary, still fast.
- `@all stop`, `@all demo on` — still binary fan-out.
- `@16 reboot` — peripheral reboots. Central's tunnel will time out (`ERR tunnel timeout`) because the peripheral never produces a terminator before restarting. That's expected; rescan after a second.

**Known limitations / notes:**
- Only one text request per peripheral in flight at a time. Central serialises by construction (one command at a time from the user).
- `@16 reboot` timeout is cosmetic — the reboot itself works; we just can't see the `OK` because the peripheral restarts before producing one.
- Tunnel overhead for a 100-byte response: ~3 round trips × ~1 ms each = a few ms. For `cfg <json>` with a fully-populated board, ~50 chunks, ~60 ms. Invisible for human-triggered commands, unsuitable for 60 Hz slider updates (that's why hot-path stays binary).

## Central M2 — binary forwarding (2026-04-20) — **confirmed on hardware**

End-to-end hot path from Central's CLI to Peripheral hardware via I2C. Tested: `@16 pi`, `@16 p`, `@16 demo`, `@16 stop`, `@all stop`, `@all demo` all working.

**New binary messages** (added to `Protocol.h` + `shared/i2c_messages.json`):
- `MSG_SET_PARAMETER (0x33)` — single parameter update, 4 data bytes (`ELEMENT_INDEX`, `PARAMETER_ID`, `VALUE` uint16 LE). Hot-path companion to the existing `MSG_SET_PARAMETERS` (all-params).
- `MSG_DEMO (0x34)` — 2 bytes (`ELEMENT_INDEX`, `ACTION`). `ELEMENT_INDEX=0xFF` (`DEMO_ALL_ELEMENTS`) targets every element on the addressed peripheral. `ACTION=0` off, non-zero on.

**Peripheral** (`src/peripheral/main.cpp`) — two new cases in the existing `onI2CReceive` dispatcher, matching the existing pattern. Parameter and demo updates hit the same paths as the text-protocol `pi`/`demo` commands.

**Central** (`src/central/main.cpp`) — full addressed-command support:
- `@<addr> <cmd> <args>` parser in `handleLine`.
- Forwarders: `fwdStop`, `fwdSetParameters`, `fwdSetParameter`, `fwdDemo`. Each parses args, builds the binary message, calls `Wire.endTransmission()`, checks status, reports `ERR <bus-status>` on failure, bumps the peripheral's `lastMissCount`.
- `@all stop` / `@all demo on|off` fan-out — inlined sends, fire-and-forget per signed-off decision (no per-device ACK wait, no per-device ERR emission). Single `OK` at the end.
- Roster check before forwarding: unknown address → `ERR no peripheral at that address` (fail fast, no bus attempt).
- Framing: every forward path emits exactly one `OK` or `ERR <reason>`. Bus errors also log a `# [fwd] ...` diagnostic.
- Help updated with a separate "Addressed (forwarded over I2C)" section.

**Testable end-to-end now via Central's CLI:**
- `@16 stop` — peripheral 16 stops.
- `@16 pi 1 0 0.5` — servo (element 1, param 0 = POSITION) goes to mid-travel. This is the path a GUI slider will take once M5 lands.
- `@16 p 1 0.2 0.8` — all params on element 1 at once.
- `@16 demo 1 on` / `@16 demo all off` — demo per element or all-elements.
- `@all stop` — every peripheral halts simultaneously.
- `@all demo on|off` — demo fan-out across all boards.

**Deferred to M3 (text tunnel):** `@<addr> cfg`, `@<addr> board`, `@<addr> whoami`, `@<addr> reboot`, `@<addr> help`, `@<addr> stop <idx>`. Anything that needs multi-line or rich text response.

## Peripheral I2C slave — binary handlers (2026-04-20)

Unblocks the Central M1 roster from showing empty types. Handles all three messages defined in `shared/i2c_messages.json`.

- **Wire slave setup** (`setupI2CSlave()` in `src/peripheral/main.cpp`) — `Wire.begin(addr, SDA=8, SCL=9, 100kHz)` + `onReceive` + `onRequest`. Address validated in 0x08..0x77; out-of-range addresses skip setup with a log.
- **`MSG_GET_CONFIG` (0x30)** — `onReceive` sets a pending-response flag; `onRequest` writes `elementCount` + one `type_id` byte per element in index order. Empty config writes `0` and nothing else.
- **`MSG_STOP` (0x32)** — `onReceive` calls `allStop()` on every Element. Parameter writes and demo state both clear.
- **`MSG_SET_PARAMETERS` (0x31)** — `onReceive` parses `ELEMENT_INDEX` + `PARAMETERS_LENGTH` + uint16-LE data bytes, validates count matches `parameterCount() * 2`, then delegates to `Element::setParameters()`. Malformed messages drain and drop.
- All I2C handlers run in the Wire slave task; keep work short (no LittleFS, no long prints). Parameter stores on `_params[]` are atomic on the 32-bit MCU; the main-loop update cycle picks them up on its next tick.
- I2C pins explicit (GPIO 8 / 9) so wiring is unambiguous and switching to C6 (different defaults) is a one-line change.
- `pi <idx> <pid> <val>` for single-parameter set does **not** have a binary equivalent yet — will route through the text tunnel in M3/M4 as planned. Slider drag performance is therefore the test-through-Central cost we accept until then.

**Testable end-to-end now via Central M1:**
- Power a Peripheral at address 16.
- On Central: `scan` → sees 16. `list` → shows `16 last_seen=.. types=[2] configFetched=yes` once `fetchConfig` succeeds.
- `STOP` and `SET_PARAMETERS` paths are scaffolded and ready for Central M2 to exercise them.

## Central M0+M1 complete (2026-04-20)

`src/central/main.cpp` fleshed out from the stub to cover milestones M0 and M1 of `CENTRAL_PLAN.md`. Mirrors the Peripheral's CLI framing (`OK`/`ERR`, `# ` log prefix, `> ` prompt, 4096-byte CLI buffer, strtok-based dispatcher). No libraries added.

- **M0 commands:** `whoami` (reports `role=central peripherals=<N> fw=v0.1`), `help`/`?`, `reboot`, unknown → `ERR unknown command`.
- **M1 I2C master:** `Wire.begin()` at 100 kHz on default S3 pins (SDA=8, SCL=9). Boot-time scan seeded before first `> `.
- **Roster:** `PeripheralRec roster[MAX_PERIPHERALS=32]` — flat struct array, `inUse` flag, no dynamic allocation. `lastMissCount` drives staleness (>= 3) and forgetting (>= 6).
- **`doScan()`:** probes 0x08..0x27 (8..39 decimal). On new address: alloc + `fetchConfig()` after the sweep. On existing: refresh `lastSeenMs`, clear miss count. Misses: bump count; forget at 6. Logs a single `# [scan] found 8 17 ...` summary line per scan.
- **`fetchConfig()`:** sends `MSG_GET_CONFIG (0x30)`, requests `1+MAX_ELEMENTS_PER_BOARD` bytes, reads `CONFIG_LENGTH` then type bytes. Drains any extra bytes. Sets `configFetched` only on an exact-length read. All failures leave `configFetched=false` and log a `# [cfg]` line.
- **M1 commands:** `scan` (immediate), `list` (one line per in-use entry — `<addr> last_seen=<ms>ms types=[..] configFetched=yes|no [stale]`), `forget <addr>` (idempotent), `scan_interval <ms>` (runtime only, 0 = off, default 0).
- `LOOP_DELAY_MS = 5` for USB CDC responsiveness.
- Decimal-only addresses in all user-facing output (signed-off decision).

**Not built in this session** — PlatformIO was unavailable to the agent; Dave will need to run `pio run -e peripheral && pio run -e central` locally before flashing. Code was written against the existing patterns in `src/peripheral/main.cpp` and the APIs used (`Arduino.h`, `Wire.h`, `Protocol.h`) are all already in use elsewhere in the project.

**Deferred to M2+ (not attempted):** `@<addr>` parsing, text tunnel, binary-hot-path forwarding, Peripheral-side I2C slave handler. `fetchConfig()` will therefore currently fail silently against the existing Peripheral firmware (which doesn't respond to I2C yet) — that's expected; the roster still tracks addresses, types stay empty, `configFetched` stays `no`.

**One thing worth raising before M2:** the Peripheral currently has no I2C slave at all. `doScan()` probes ACK-level only, which will work on any I2C-addressed device, but `fetchConfig()` needs the Peripheral side of M3 (or at minimum a `MSG_GET_CONFIG` slave handler) before we can see real types on Central. Might be worth sequencing a lightweight "Peripheral responds to `MSG_GET_CONFIG`" slice before M2's binary forwarding lands, so we can end-to-end test the roster.

---

## Current state (2026-04-20)

Only scaffolding so far:
- `shared/control_schema.json` — 4 element types declared: `PWM_MOTOR`, `SERVO_MOTOR`, `SERVO_ARM`, `CONTINUOUS_SERVO`
- `shared/example_elements.json` — example board config using 3 of the 4 types
- `shared/i2c_messages.json` — 3 messages: `GET_CONFIG`, `SET_PARAMETERS`, `STOP`

No C++ yet. No GUI yet. No PlatformIO project yet.

---

## Decisions so far

- **2026-04-20** — PlatformIO layout: **one project, two envs** (`[env:central]` + `[env:peripheral]`), shared code in `lib/` or `src/shared/` (TBD). Keeps protocol/schema/Element base class shared.
- **2026-04-20** — Target board: **Seeed XIAO ESP32-C3** for both Central and Peripheral. (Briefly considered RP2040, but Dave is more familiar with ESP32.) Matches the original `OVERVIEW.md`. Rationale: cheap, native USB, WiFi/BT on-chip keeps Future Plans alive.
- **2026-04-20** — Central language: **C++** (keeps one toolchain for the whole subproject). May revisit later if CircuitPython feels better on Central.
- **2026-04-20** — **All parameters are floats.** No per-parameter type annotation needed in the schema.
- **2026-04-20** — `SERVO_MOTOR` gets parameter `POSITION`.
- **2026-04-20** — Schema field rename: `id` → `type_id` (integer), `id_string` → `type_name` (string). Per-board element index becomes `index`.
- **2026-04-20** — `GET_CONFIG_RESPONSE` returns only Element `type_id` list (cheap summary for GUI building). Full per-element config (pins etc.) is only available over USB Serial as JSON.
- **2026-04-20** — Peripheral I2C address stored in `config.json` *or* in a separate small file so `config.json` is portable between boards. (Two-file approach: `config.json` for Elements, `board.json` for I2C address + board-specific settings.)
- **2026-04-20** — **Parameters are `uint16_t` on the wire AND internally.** Semantic mapping: `[0, 65535]` ↔ `[0.0, 1.0]`. Two bytes per parameter on I2C, little-endian. Elements store parameters as `uint16_t` and convert to hardware values (PWM duty, servo microseconds, etc.) at the moment of writing to hardware. Bidirectional Elements (e.g. `CONTINUOUS_SERVO`) interpret `0.0/0.5/1.0` as reverse/rest/forward via their config (`MIN_SPEED`/`REST_SPEED`/`MAX_SPEED`). No float handling in the hot path.

**Pending OVERVIEW.md update:** `OVERVIEW.md` already says ESP32, so it's not stale. Minor future refinement: narrow "ESP32" → "ESP32-C3 (XIAO form factor)" once we're sure no Peripheral will need more GPIO than XIAO exposes (~11 pins).

---

## Notes on XIAO ESP32-C3

- **Framework:** arduino-esp32 (Espressif's Arduino core) via PlatformIO. Mature, lots of examples.
- **Native USB:** ESP32-C3 has USB-Serial/JTAG native; the XIAO board exposes USB-C. Good for USB Serial without an extra chip. For USB MIDI on Central: TinyUSB support is available via the Arduino core.
- **WiFi + BT 5 LE:** on-chip. Keeps the `OVERVIEW.md` Future Plans (swap I2C for WiFi/Matter) viable without a co-processor.
- **Pin budget:** XIAO ESP32-C3 breaks out ~11 GPIO (D0–D10). I2C uses 2; that leaves ~9 for motors/servos per Peripheral. Enough for a small handful of Elements per board — size the system around this.
- **I2C:** hardware I2C peripheral; `Wire` library works for both master and slave (slave mode on ESP32-C3 is functional on recent arduino-esp32).
- **Filesystem:** LittleFS is the standard for on-device config storage.
- **Single core:** RISC-V single core @ 160 MHz. Fine for this workload (I2C + servo/PWM updates), but worth keeping `update()` loops tight and avoiding long blocking calls.

---

## Open questions

### Resolved (2026-04-20)

- ✅ Q1 SERVO_MOTOR → `POSITION`.
- ✅ Q3 GET_CONFIG_RESPONSE is intentionally a bare list of `type_id`s over I2C. Rich config lives in JSON over USB Serial.
- ✅ Q4 Field renames adopted: `type_id`, `type_name`, `index`.
- ✅ Q5 Peripheral I2C address: stored on-device. **Two-file plan:** `config.json` (Elements, reusable between boards) + `board.json` (I2C address and anything board-specific). Subject to adjustment once we see how this plays out.
- ✅ Q8 Central language: C++.

### Open

✅ **Q2 — Parameter encoding.** Resolved: **`uint16_t` everywhere** (wire + internal). Mapping `[0, 65535]` ↔ `[0.0, 1.0]`. Little-endian on I2C. Element converts to hardware values at the point of writing to hardware. Bidirectional Elements (`CONTINUOUS_SERVO`, etc.) interpret `0.5` as rest via their config.

**Consequences:**
- `Element` base class signature uses `uint16_t* vals` (not floats).
- The project commits to the convention: *every parameter is semantically in [0.0, 1.0] on the wire*. New Element types must normalize their hardware range into their config.
- USB Serial CLI (`p` command) will accept floats `[0.0, 1.0]` for human ergonomics, but convert to `uint16_t` immediately.
- GUI sends `uint16_t` directly (or converts on send).

**Q6 — Parameter count validation in SET_PARAMETERS.** Currently the message carries `ELEMENT_ID` + `PARAMETERS_LENGTH` (bytes). With a fixed per-parameter byte count (2 or 4), length implicitly fixes count. Propose: keep `PARAMETERS_LENGTH` as-is, and have the Peripheral validate that the count matches the target Element's expected parameter count (from schema). Malformed → silent drop + log over USB Serial (see Q7).

**Q7 — Error handling.** Proposed minimal policy:
- Malformed I2C messages: silent drop + log a one-line message over USB Serial with reason.
- `SET_PARAMETERS` on unknown element index or wrong param count: silent drop + log.
- No I2C error-response messages in v1 (keeps Peripheral I2C state machine simple). Can add later if diagnostics become a pain point.

### Hardware / firmware shape

**Q9 — Config storage on device.** Proposal: LittleFS with two files — `config.json` (Elements, portable between boards) + `board.json` (I2C address, board-specific settings). Loaded at boot.

**Q10 — USB Serial CLI grammar on Peripheral.** Draft grammar, open to edits:
- `stop` / `s` — all-stop all Elements.
- `p <index> <f1> [f2 ...]` — set parameters on an Element. Floats in `[-1,1]` (or whatever Q2 lands on).
- `cfg` — dump current config.json.
- `cfg <json>` — replace config.json with supplied JSON, save to LittleFS, reboot.
- `board` — dump board.json.
- `board <json>` — replace board.json.
- `demo <index>` — run one Element's built-in demo cycle.
- `demo all` — run all Elements' demo cycles in sequence.
- `list` — list loaded Elements with index + type_name.
- `?` / `help` — print command summary.

**Q11 — Element base class shape.** Proposed (matches `OVERVIEW.md`):
    ```cpp
    class Element {
    public:
      virtual void configure(const JsonObject& cfg) = 0;
      virtual void setParameters(const float* vals, uint8_t n) = 0;
      virtual void update(uint32_t ms) = 0;  // called every loop tick
      virtual void allStop() = 0;
      virtual void demo() = 0;               // cycle through functions
      virtual const char* typeName() const = 0;
      virtual uint8_t typeId() const = 0;
    };
    ```

### GUI

12. **HTML/JS GUI:** WebSerial is Chromium-only. That probably fine for a studio tool? Alternative: a small Python/Electron wrapper.
13. **GUI connects to Central or Peripheral?** Per `OVERVIEW.md`, either. My lean: start with GUI-to-Peripheral (direct, for config + direct control), then add GUI-to-Central (for network overview + broadcast commands).

---

## Proposed build order

Not final — just a sketch to align on.

1. **Resolve schema/protocol open questions (Q1–Q7).** Update `shared/*.json` to canonical forms. No code yet.
2. **Scaffold PlatformIO project** — `platformio.ini` with two envs, shared `lib/AOElectronics/` for protocol + Element base + config loader.
3. **Peripheral, minimum viable:**
   - Boot, mount LittleFS, read `config.json`.
   - `Element` base class + one concrete type (start with `PWM_MOTOR` — simplest).
   - Main loop calling `update()` on all elements.
   - USB Serial CLI: `stop`, `p <index> <f> <f>...`, `cfg`, `demo <index>`.
4. **I2C, Peripheral side:** respond to `STOP`, `SET_PARAMETERS`, `GET_CONFIG`.
5. **Central, minimum viable:**
   - Enumerate I2C bus on boot.
   - Forward `STOP` broadcast.
   - Forward `SET_PARAMETERS` from USB Serial → I2C.
6. **HTML/JS GUI** (WebSerial) — connect to Peripheral directly, show config, slide parameters.
7. **Add more Element types** (`SERVO_MOTOR`, `SERVO_ARM`, `CONTINUOUS_SERVO`) as schema + C++ in parallel.
8. **Central: USB MIDI** → parameter updates. CC-to-parameter mapping mechanism TBD.
9. **GUI enhancements:** device network view, broadcast commands via Central.

---

## Status — scaffold round 1 (2026-04-20)

Applied all resolved decisions to shared schemas and scaffolded the PlatformIO project with one Element (PWM_MOTOR) as the reference pattern.

**In the repo now:**

- `shared/control_schema.json`, `shared/example_elements.json`, `shared/i2c_messages.json`, `shared/example_board.json` — rewritten with renames (`type_id`, `type_name`, `index`), SERVO_MOTOR gains `POSITION`, encoding spec is explicit (uint16 LE, [0,65535]↔[0,1]).
- `platformio.ini` — one project, two envs (`peripheral`, `central`), board `seeed_xiao_esp32c3`, native USB CDC, LittleFS, deps on ArduinoJson v7 and ESP32Servo.
- `lib/AOElectronics/` — shared library:
  - `Element.h` — abstract base class (configure, update, setParameters, allStop, startDemo, parameterCount, typeId, typeName).
  - `Protocol.h` — I2C message IDs, encoding constants, type IDs, `paramToFloat` / `floatToParam` helpers.
  - `PWM_MOTOR.h`/`.cpp` — first concrete Element. Uses arduino-esp32 `ledcAttach`. Ramps between values over `RAMP_TIME`.
  - `ElementFactory.h`/`.cpp` — string-to-Element. Only `PWM_MOTOR` registered for now; add more as they're implemented.
  - `ConfigLoader.h`/`.cpp` — mounts LittleFS, loads `/board.json` and `/config.json`.
  - `library.json` — PIO library manifest.
- `src/peripheral/main.cpp` — boots, loads config, runs update loop, USB Serial CLI (`list`, `cfg`, `board`, `stop`/`s`, `p`, `demo`, `help`).
- `src/central/main.cpp` — stub so the env compiles.
- `data/config.json` — single PWM_MOTOR on pin 3 for first flash-and-test.
- `data/board.json` — I2C address 0x10.

**Not yet implemented (deliberately):**
- I2C slave side of the Peripheral — `pumpSerial()` works, I2C bus handling is next.
- `cfg <json>` and `board <json>` CLI writes (reading is in; writing needs care around LittleFS write cycles + reboot).
- Other Elements: `SERVO_MOTOR`, `SERVO_ARM`, `CONTINUOUS_SERVO`. Deferred until the `PWM_MOTOR` pattern is reviewed.
- Central firmware (anything beyond the stub).
- HTML/JS GUI.
- Unit tests / hardware-in-the-loop tests.

**Known unknowns / caveats:**
- `seeed_xiao_esp32c3` board ID in PlatformIO is correct to the best of my knowledge — if `pio run -e peripheral` complains, we may need to tweak the board or pin in a specific esp32 platform version.
- ESP32-C3 I2C slave mode in arduino-esp32 has had rough edges historically. Worth confirming slave support in the installed platform version before writing the I2C handler.
- `Element*` pointers are heap-allocated via factory; currently never freed (lifetime = program). Fine for v1.

## Review round 1 → refactor (2026-04-20)

Dave reviewed the pattern and requested:

- **Demo is continuous, not time-bounded.** Base class owns `demoActive` + `startDemo()`/`stopDemo()`. `update(dtMs)` in base dispatches to `updateControl(dtMs)` or `updateDemo(dtMs)` (both pure virtual). Template Method pattern.
- **Split `configure()` and `initialise()`.** `configure(JsonObjectConst)` only reads values; `initialise()` does hardware setup (`ledcAttach` etc.). ConfigLoader calls `configure()`; `main.cpp` does a separate `initialiseElements()` pass. Rationale: easier to debug where bringup fails.
- **`update()` takes `dtMs` (delta), not `nowMs`.** Main loop tracks `lastUpdateMs` once; Elements stop bookkeeping time.
- **`demo` CLI explicit on/off:** `demo <idx|all> on|off`. No auto-stop.
- **`advanceDemoAll` gone.** `demo all on` just calls `startDemo()` on every Element.
- **Main loop delay** of `LOOP_DELAY_MS = 50` ms (variable, not #define, may become config).
- `setParameters()` no longer cancels demo — demo is an explicit mode; `stop`/`allStop` ends it.

Element class hierarchy now:
```cpp
class Element {
  // lifecycle
  virtual bool configure(JsonObjectConst) = 0;  // values only
  virtual bool initialise() = 0;                 // hardware
  // dispatcher (non-virtual)
  void update(uint32_t dtMs);
  // parameters / safety / demo switch (base-owned)
  virtual bool setParameters(...) = 0;
  virtual void allStop() = 0;
  void startDemo(); void stopDemo(); bool isDemoActive() const;
  // type info
  virtual uint8_t parameterCount() const = 0;
  virtual uint8_t typeId() const = 0;
  virtual const char* typeName() const = 0;
protected:
  virtual void updateControl(uint32_t dtMs) = 0;
  virtual void updateDemo(uint32_t dtMs) = 0;
  virtual void onDemoStart() {} virtual void onDemoStop() {}
};
```

## SERVO_MOTOR + Central plan (2026-04-20)

**SERVO_MOTOR** implemented — `lib/AOElectronics/SERVO_MOTOR.{h,cpp}` + registered in `ElementFactory`. Uses ESP32Servo (already in deps), 50 Hz PWM, 500-2500 µs pulse range. `allStop()` holds position (snapping to 0 could bang into a mechanical stop). Demo: 5 s triangle sweep. No RAMP_TIME in schema — revisit if needed. Compiles but not yet hardware-tested.

**Central plan signed off.** See `CENTRAL_PLAN.md` for the full architectural roadmap with Dave's 9 decisions recorded. Key points:
- Addressing: `@<addr> <cmd>`, decimal-only (`@16 cfg`), with `@all` for fire-and-forget fan-out.
- Peripheral addresses constrained to 8-39 (0x08..0x27); scans probe 32 slots.
- Text tunnel (new I2C ops 0x40 `TEXT_WRITE` / 0x41 `TEXT_READ`) carries all non-realtime commands. Realtime (`p`, `pi`, `stop`, `GET_CONFIG`) stays binary.
- `@all` is fire-and-forget; individual NAKs logged not failed.
- Scan on request only by default; `scan_interval <ms>` runtime setting (0 off).
- No Central persistence in v1; leave room for `midi_map.json` at M7.
- Central stays on ESP32-S3 (USB-OTG needed for TinyUSB MIDI composite). Peripherals may move to C6.
- 8 milestones: M0 `whoami` → M1 scan/roster → M2 binary-hot-path forward → M3 Peripheral text tunnel → M4 Central text tunnel → M5 GUI Central role → M6 USB MIDI → M7 CC mapping → M8 polish.

## GUI round 3 — narrower cards, summary sliders, `pi` command (2026-04-20)

**Firmware:**
- **Element base refactor.** The base class now owns parameter storage (`uint16_t _params[MAX_PARAMS=8]`) and provides concrete `setParameters()` + `setParameter(index, val)` + `parameter(index)` accessor + `onParametersChanged()` hook. `setParameters` is virtual-not-pure; Elements may override for validation.
- **PWM_MOTOR simplified.** Dropped its own `targetParam` member — reads `parameter(0)` directly in `updateControl`. `allStop` zeroes `_params[0]` instead.
- **New `pi` command** — `pi <idx> <pid> <val>` sets a single parameter by 0-based index. Documented in `shared/serial_protocol.md`.
- Help text and protocol docs updated.

**GUI:**
- **Multi-column layout.** `.devices` is now `grid-template-columns: repeat(auto-fill, minmax(340px, 1fr))` — several Peripheral cards fit side-by-side on wide screens.
- **Summary params section** inside each peripheral card: one slider per (element, parameter). Always visible.
- **Peripheral-level Config toggle.** Default expanded; when clicked, adds `.compact` class to the device card, hiding board + element cards (keeping header + summary sliders).
- **Slider sync.** Each param has a record `{ value, elSlider, elValEl, sumSlider, sumValEl, sendState }`. Moving either slider updates both and sends `pi <idx> <pid> <val>`.
- **Per-parameter throttling.** Replaces the per-element throttle from round 2. One command in flight per param; fast drags overwrite the pending value.
- Summary rebuilds on Add/Remove element.
- Tightened typography: smaller fonts, 2-col config grid, reduced padding. Cards fit much more in the same space.

**Not yet:**
- Reorder elements (drag to reorder) — currently add-to-end only.
- Per-param rename / custom labels.
- Multiple Peripheral connections from one GUI instance.

## GUI round 2 — runtime controls (2026-04-20)

Building on round 1 after a successful hardware test. Additions:

**Firmware:**
- `stop <idx>` variant to stop a single Element (uses `Element::allStop()`).
- Protocol doc updated.

**GUI:**
- **Command input** above the log panel — types run as real commands through the protocol layer. `reboot` special-cased (no OK expected).
- **Stop all** button in the top bar.
- **Per-element demo switch** — `wa-switch` labelled "Demo", sends `demo <idx> on|off`, reverts on ERR.
- **Per-element stop button** — sends `stop <idx>`, also resets slider UI to 0 and flips demo switch off.
- **Parameter sliders** — one `wa-slider` per parameter, 0-1 range with 0.01 steps, tooltip on. `input` event drives sends with **latest-wins throttling**: one command in flight per element max; fast drags overwrite the pending value rather than piling up in the queue.
- **Hide config toggle** (`wa-switch` in device header) — a compact mode that hides element config blocks and the "Save elements" button. Board settings stay visible.
- **Add-element dialog fix** — Web Awesome 3.5 uses `dialog.open = true/false` + `wa-after-hide` event, not Shoelace-era `.show()` / `sl-after-hide`. Previous round was non-functional.
- Compact element card layout: header row with demo/stop/remove actions; params with inline value readout; config in a grid that collapses when hide-config is on.

**Known limitations still in place:**
- No auto-reconnect after reboot.
- No "unsaved changes" indicator.
- Element index is auto-renumbered 1..N on save.
- Central firmware still a stub.

## GUI + protocol round (2026-04-20)

Went read+write in one pass. Three moving parts landed together: firmware protocol rework, new shared docs, browser GUI.

**Protocol** (new: `shared/serial_protocol.md`):
- Every command terminates with `OK` or `ERR <reason>` on its own line.
- Log lines always start with `# ` — hosts filter them out.
- Prompt (`> `) remains human-friendly (no trailing newline); GUI strips leading `> ` sequences from every line before processing.
- New commands: `whoami`, `cfg <json>`, `board <json>`, `reboot`.
- `CLI_BUF_SIZE` bumped to 4096 bytes so full-board `cfg <json>` payloads fit.

**Firmware changes** (`src/peripheral/main.cpp`, plus log-prefix `# ` migration in `PWM_MOTOR.cpp`, `ConfigLoader.cpp`, `ElementFactory.cpp`):
- `writeFileAtomic()` helper: writes to `<path>.new`, then renames — avoids half-written configs on failure.
- JSON validation via ArduinoJson before write.
- All previously-unmarked log lines now prefixed `# `.

**Shared docs:**
- `shared/serial_protocol.md` — authoritative text-protocol spec.
- `CONFIG_TYPES.md` — mapping from schema types (`pin_id`, `milliseconds`, `integer`) to GUI input controls. Extension guide included.

**GUI** (`gui/`, Web Awesome 3.5.0 via CDN — no build step):
- `index.html` + `styles.css` — layout with topbar, device cards, log panel.
- `serial.js` — WebSerial wrapper; line-oriented; subscriber-based.
- `protocol.js` — command serialization, OK/ERR parsing, log splitting, `> ` stripping, 3-5s timeouts.
- `ui-schema.js` — factory producing `wa-input` controls per schema type.
- `app.js` — orchestration: load schema, connect, `whoami`, fetch board+config, render, save, add/remove, reboot.

**Scope delivered:**
- Connect/disconnect to USB serial via browser port picker.
- Identify device via `whoami`.
- For a Peripheral: board card with I2C address; Elements section with one sub-card per Element rendered from schema.
- Add Element (dialog with type picker) / Remove Element.
- Save board, save elements (separate buttons per section), Reboot, Reload.

**Not done / known gaps:**
- Central GUI side is a placeholder — waits for Central firmware.
- No parameter control (sliders) yet — belongs to a later round once I2C live control is wired.
- No auto-reconnect after reboot (user clicks Connect again).
- No visual "unsaved changes" indicator.
- Element index is auto-managed (renumbered 1..N on save); manual index editing not exposed.

**Run:**
```
cd I2CElectronics
python3 -m http.server 8000
# open http://localhost:8000/gui/ in Chrome/Edge/Arc/Brave
```

## arduino-esp32 v2.x vs v3.x — LEDC API (2026-04-20)

Platform `espressif32 @ ^6.9.0` (and similar current PIO releases) still ships arduino-esp32 **v2.x**. The newer `ledcAttach(pin, freq, res)` / `ledcWrite(pin, duty)` API is v3.x only. The v2.x LEDC API uses explicit channels (`ledcSetup(ch, freq, res)` / `ledcAttachPin(pin, ch)` / `ledcWrite(ch, duty)`), which is incompatible with v3.x's pin-based `ledcWrite`.

**Decision:** use Arduino's `analogWrite(pin, duty)` in PWM_MOTOR instead. Works on both v2.x and v3.x; arduino-esp32 handles LEDC channel allocation internally. Default PWM frequency (~1 kHz) and 8-bit duty are fine for DC motors. If we later need explicit frequency control, revisit with per-version code.

**Also removed** `-DARDUINO_USB_MODE=0` from build flags — the `esp32-s3-devkitc-1` board profile already sets `ARDUINO_USB_MODE`, and our override was only producing redefined-warnings. Either mode gives USB Serial; the board default works.

## ESP32-S3-Zero boot-loop fix (2026-04-20)

Initial flash produced a continuous reset loop (only ROM bootloader output, repeated). Root cause: `esp32-s3-devkitc-1` board profile defaults assume **OPI PSRAM**, but the S3-Zero uses ESP32-S3FN4R2 which has **QSPI PSRAM**. PSRAM init failed → second-stage bootloader crashed → reset loop.

Fix added to `platformio.ini`:
```
board_build.arduino.memory_type = qio_qspi
board_build.flash_mode = qio
```

When switching board profiles in future (XIAO C6 etc.), these overrides may need revisiting — C6 has no PSRAM at all, so `memory_type` may not apply.

## Current board: Waveshare ESP32-S3-Zero (temporary) — 2026-04-20

While XIAO ESP32-C6 boards are in transit, using a Waveshare ESP32-S3-Zero for initial hardware bring-up. `platformio.ini` changes:
- `board = esp32-s3-devkitc-1` (generic S3 profile; S3-Zero fits)
- `board_upload.flash_size = 4MB` + `board_build.flash_size = 4MB` (S3-Zero has 4MB flash, no PSRAM on standard variant)
- `-DARDUINO_USB_MODE=0` (S3 uses native USB OTG, not HW CDC+JTAG)
- `platform = espressif32 @ ^6.9.0` pinned — need arduino-esp32 v3.x for `ledcAttach()`

When C6 arrives, revert per the comment block in `platformio.ini`.

`data/config.json` still uses `PWM_PIN: 3` — GPIO3 is fine on both S3-Zero and C3/C6, so nothing to change there.

## ESP32-C6 portability note

Dave may switch to ESP32-C6 (XIAO form factor). Code impact: essentially zero.
- Change `board = seeed_xiao_esp32c3` → `seeed_xiao_esp32c6` in `platformio.ini`.
- USB CDC build flags stay the same.
- `ledcAttach`, `Wire`, `LittleFS`, `Serial` APIs identical.
- **Pin numbers in `config.json` are raw GPIO and differ between C3 and C6 for the same silkscreen pad.** That's a per-board-variant `config.json` concern, not a code concern.
- C6 adds 802.15.4 (Thread/Matter) on-chip — strengthens the Future Plans note (WiFi/Matter swap) further.

Broader portability note: nothing currently is wrapped behind a `#ifdef`. If a future Peripheral board isn't ESP32-family, the things that would need wrapping are `ledcAttach`/`ledcWrite` (use PWM abstraction), `LittleFS` (abstract filesystem), and potentially the servo library. Not urgent — address when it becomes real.

## Next review checkpoint

Refactored. Same files — re-review if wanted:
- [Element.h](lib/AOElectronics/Element.h)
- [PWM_MOTOR.h](lib/AOElectronics/PWM_MOTOR.h) / [.cpp](lib/AOElectronics/PWM_MOTOR.cpp)
- [ConfigLoader.cpp](lib/AOElectronics/ConfigLoader.cpp) (calls configure only, not initialise)
- [peripheral/main.cpp](src/peripheral/main.cpp) (new `initialiseElements()` pass, dt-based update, new demo CLI, LOOP_DELAY_MS)

After Dave OKs:
1. Implement `SERVO_MOTOR`, `CONTINUOUS_SERVO`, `SERVO_ARM`.
2. Add the I2C slave handler on the Peripheral.
3. Begin on the Central.

