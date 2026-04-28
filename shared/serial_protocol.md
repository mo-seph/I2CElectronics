# I2CElectronics — USB Serial text protocol

This protocol is used between a host (human on a terminal, or the HTML/JS
GUI, or a script) and a Central or Peripheral device over USB Serial.
It is **distinct from the I2C binary protocol** (see `i2c_messages.json`),
which is used between Central and Peripherals on the I2C bus.

The text protocol is line-oriented: every command and every response is
a line terminated by `\n`.

## Framing convention

1. **Command** — one line sent by the host, e.g. `whoami\n`.
2. **Response data** — zero or more lines emitted by the device. These
   contain whatever the command is asked to produce (JSON dump, role info,
   list output, etc.).
3. **Terminator** — exactly one of:
   - `OK` — command succeeded. Any preceding data lines are the response.
   - `ERR <reason>` — command failed. Any preceding data lines should be
     ignored as partial output.
4. **Log lines** can appear at any time, asynchronously, and always begin
   with `# ` (hash + space). Hosts **must** ignore lines starting with
   `# ` when collecting responses. Firmware uses this prefix for all
   diagnostic output (boot banner, element errors, etc.).
5. **Prompt** — the device emits `> ` after processing each command, for
   human ergonomics. Hosts can ignore this; it has no semantic meaning.

A GUI reading lines can therefore use this loop:

```
send(command + "\n")
response = []
while True:
    line = readline()
    if line.startswith("# "): continue          # log
    if line == "OK":         return response    # success
    if line.startswith("ERR "): raise(line[4:]) # failure
    if line == "> ":         continue           # prompt; ignore
    response.append(line)
```

## Commands

### `whoami`
Identify the device. One data line, then `OK`.

Peripheral response:
```
WHOAMI role=peripheral id=<i2c_address>
OK
```
Central response:
```
WHOAMI role=central
OK
```

### `list`
List loaded Elements.

```
<n> Element(s) loaded:
  <index>: <type_name> (type_id=<n>, params=<n>, demo=on|off)
  ...
OK
```

### `cfg`
Dump the current `config.json` (Element list with per-element config).

```
<json array>
OK
```

### `cfg <json>`
Replace `config.json` on device flash with `<json>` (single-line compact
JSON array). Does **not** auto-reload the running Elements — call
`reboot` to apply.

```
OK
```
or
```
ERR invalid json
ERR write failed
```

### `board`
Dump the current `board.json` (board-specific settings, e.g. I2C address).

```
<json object>
OK
```

### `board <json>`
Replace `board.json` on device flash. `reboot` to apply.

```
OK
```
or `ERR invalid json` / `ERR write failed`.

### `reboot`
Restart the device. No terminator — the connection will drop and the
device boot sequence follows. Hosts should wait and reconnect.

### `stop` / `s` [optional `<index>`]
All-stop every Element, or just the one at `<index>`. Ends any active demo
on the affected Element(s).

```
stopped        # or: stopped <index>
OK
```

### `p <index> <f1> [f2 ...]`
Set **all** parameters on a specific Element. Values are floats in
[0.0, 1.0]; the firmware converts to uint16 internally. The count must
match the Element's parameter count.

```
OK
```
or `ERR ...`.

### `pi <index> <parameter_id> <f>`
Set **a single** parameter on a specific Element, by zero-based
parameter index. `parameter_id=0` is the first entry of the Element's
`parameters` array in `control_schema.json`. Value is a float in
[0.0, 1.0]. Useful when a host is streaming updates for one parameter
at a time (e.g. sliders in a GUI).

```
OK
```
or `ERR setParameter rejected` (index out of range).

### `demo <index|all> on|off`
Turn an Element's continuous demo sequence on or off.

```
demo <index>: on|off
OK
```

### `help` / `?`
Print a human-readable command summary.

```
(multi-line help text)
OK
```

## Addressed commands (Central only)

When talking to a Central, commands prefixed with `@<addr>` are forwarded
to the Peripheral at that address over I2C. The Peripheral's own text
protocol is not used for the forward path — the Central translates the
text command into the appropriate binary I2C message.

### Addressing syntax
- `@<addr> <cmd> <args>` — decimal address only (e.g. `@16 pi 1 0 0.5`)
- `@all <cmd> <args>` — fan-out to every roster entry; fire-and-forget

### Binary hot-path commands
One I2C transaction each. Used by GUI sliders and anything that needs
low latency.

- `@<addr> stop` → `MSG_STOP` (0x32)
- `@<addr> p <idx> <f1> [f2 ...]` → `MSG_SET_PARAMETERS` (0x31)
- `@<addr> pi <idx> <pid> <f>` → `MSG_SET_PARAMETER` (0x33)
- `@<addr> demo <idx|all> on|off` → `MSG_DEMO` (0x34)
- `@all stop` → iterate roster, send `MSG_STOP` to each (fire-and-forget)
- `@all demo on|off` → iterate roster, `MSG_DEMO` with `ELEMENT_INDEX=0xFF`

### Text tunnel commands
Anything else — `cfg`, `board`, `whoami`, `reboot`, `help`, `list`,
`stop <idx>`, etc. — rides through `MSG_TEXT_WRITE` (0x40) + `MSG_TEXT_READ`
(0x41). The Peripheral's response (including its OK/ERR terminator) is
streamed back to the Central's USB Serial verbatim. See §Text tunnel below
for the framing details.

Examples:
- `@16 cfg` → dumps the peripheral's config.json on Central's Serial
- `@16 cfg {"...":...}` → writes a new config.json to the peripheral
- `@16 whoami` → peripheral replies with its own WHOAMI line
- `@16 reboot` → peripheral resets (tunnel may time out; caller should
  rescan after a second)

### Error cases
- Unknown address not in roster: `ERR no peripheral at that address`
- Bad address syntax: `ERR invalid address`
- I2C NAK / timeout on forward: `ERR <bus-status>` (and Central bumps the
  peripheral's miss count — next scan may mark stale / forget)
- Command not yet supported over binary: `ERR cmd not supported`

## Text tunnel (I2C) — framing

The text tunnel is an I2C transport for arbitrary Peripheral text
commands. Central and Peripheral use two binary opcodes (0x40 / 0x41)
to chunk request and response bytes; the Peripheral's own text
dispatcher runs the command in main-loop context and captures output
into a buffer the Central reads back.

**Request** — Central sends `MSG_TEXT_WRITE` chunks:

```
[0x40][offset:u16 LE][length:u8][more:u8][data: length bytes]
```

Offsets start at 0 on the first chunk. `more=0` on the final chunk
triggers the Peripheral to run the assembled line through its command
dispatcher.

**Response poll** — Central sends `MSG_TEXT_READ`:

```
[0x41][offset:u16 LE][maxlen:u8]
```

Peripheral replies in the next I2C read:

```
[length:u8][more:u8][status:u8][data: length bytes]
```

- `status = 0` (`TEXT_STATUS_IN_PROGRESS`) — peripheral still processing;
  Central polls again (short backoff recommended).
- `status = 1` (`TEXT_STATUS_OK`) — command finished, terminator line
  was `OK`. Data contains the command's full output.
- `status = 2` (`TEXT_STATUS_ERR`) — command finished with `ERR`.
- `more = 1` — more bytes available past this chunk; Central increments
  `offset` and polls again.

**Rules**
- Only one text request per Peripheral at a time; Central serialises.
- A new request (Central sends `TEXT_WRITE` with `offset=0`) resets the
  Peripheral's tunnel state — any in-progress response is discarded.
- Empty / whitespace-only lines get a synthetic `ERR empty command`
  response so every command completes with a terminator.
- Max chunk payload: `TEXT_CHUNK_MAX = 64` bytes (fits well inside the
  arduino-esp32 v2.x default 128-byte Wire buffer with header headroom).
- Max request/response buffer: 4 KB each — same as the Peripheral's
  native `CLI_BUF_SIZE`.

## Notes

- Line ending is `\n` (LF). Firmware tolerates stray `\r` by ignoring it.
- `cfg <json>` and `board <json>` JSON must be **single-line / compact**
  (no embedded newlines) — newlines terminate the command line.
- Maximum line length (including JSON payloads): 4096 bytes. Larger
  payloads will be truncated; `ERR ...` will result.
- All log output uses the `# ` prefix. Historical log calls in firmware
  that lacked this prefix have been migrated; future log calls must use
  `Serial.print("# ...")` or similar.
