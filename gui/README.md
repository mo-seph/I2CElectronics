# AO Electronics GUI

Browser-based configuration tool for the I2CElectronics Peripheral and
Central devices. Talks to devices over USB Serial via the WebSerial API.

## Requirements

- **Chromium-based browser** (Chrome, Edge, Arc, Brave). WebSerial is not
  supported in Firefox or Safari.
- Served from a **secure context** — `https://` or `http://localhost`.
  Opening `index.html` directly via `file://` will not work.

## Running

From the `I2CElectronics/` directory:

```sh
python3 -m http.server 8000
```

Then open http://localhost:8000/gui/ in a Chromium browser.

Any other HTTP server works too (e.g. `npx http-server`, `caddy file-server`).
There is no build step — all files are served as-is.

## Files

| File              | Purpose                                                           |
|-------------------|-------------------------------------------------------------------|
| `index.html`      | Page skeleton; loads Web Awesome + app modules                    |
| `app.js`          | Top-level wiring: connect flow, rendering, save flow              |
| `serial.js`       | Thin WebSerial wrapper — exposes line-oriented reads/writes       |
| `protocol.js`     | Text protocol framing (see `../shared/serial_protocol.md`)        |
| `ui-schema.js`    | Factory: schema config entry → form control                       |
| `styles.css`      | Layout + domain styling                                           |

The GUI loads `../shared/control_schema.json` from the monorepo via
relative fetch — no copy is needed, and the schema stays single-source.

## How it works

1. Click **Connect** → browser shows the native port picker → user picks a
   device → port opens at 115200 baud.
2. GUI sends `whoami` to identify the device (Peripheral or Central).
3. For a Peripheral: GUI fetches `board.json` and `config.json` from the
   device and renders one card with the board settings plus one sub-card
   per Element.
4. User edits fields, clicks Save — GUI sends `board <json>` and/or
   `cfg <json>` to the device (writes to LittleFS).
5. Changes take effect on **Reboot device**.

## Protocol notes

See `../shared/serial_protocol.md` for the authoritative text protocol
spec. In short:
- Every command ends with `\n`.
- Every command produces exactly one terminator line: `OK` or `ERR <msg>`.
- Log lines begin with `# ` — the GUI filters these into the log panel.
- The firmware's human prompt (`> `) has no trailing newline and gets
  stripped by the GUI's protocol parser.

## Troubleshooting

- **"WebSerial not supported"** — you're not on a Chromium browser.
- **Port picker is empty** — the device isn't plugged in, or another
  program (e.g. `pio device monitor`) has the port open. Close it first.
- **Connect then timeout on whoami** — the firmware may be rebooting
  (wait a second) or the firmware doesn't implement `whoami` yet
  (re-flash).
- **Permission persists across reloads** — browsers remember granted
  ports. To forget: `chrome://settings/content/serialPorts`.
