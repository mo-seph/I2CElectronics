# I2CElectronics — Config entry types

Each Element in `control_schema.json` has a `config` array. Each entry has
a `type` field that tells the firmware how to interpret the value and tells
the GUI what kind of input control to render.

Keep this document and the schema in sync by hand.

## Types

| Type           | Firmware-side semantics                              | GUI control                                                 |
|----------------|------------------------------------------------------|-------------------------------------------------------------|
| `pin_id`       | Integer GPIO number (raw, not silkscreen pad)        | Dropdown of board-usable GPIOs (BOARD_PINS in ui-schema.js). Values already on a reserved pin from legacy configs appear as an annotated extra option. Duplicate pins across Elements on the same peripheral are flagged red. |
| `milliseconds` | Non-negative integer duration in ms                  | Number input, `min=0`, `step=1`, suffix "ms"                |
| `integer`      | Generic integer; `default`/`min`/`max` may be given  | Number input, `step=1`, bounds from schema if present       |
| `boolean`      | true/false. ArduinoJson `cfg["FIELD"] \| false` reads cleanly. | Native checkbox in a labelled wrap, with an "on"/"off" word next to it for at-a-glance state. |

## Conventions

- All entries **may** include `default` — used when the field is missing
  from a device's actual `config.json`. The GUI pre-fills inputs with
  `default` when creating a new Element.
- Entries **may** include `min` / `max` — hints for the GUI and for
  firmware validation. Not enforced in firmware yet; the GUI should
  clamp or warn.
- Entries **may** include `description` — shown as a hint/tooltip in the
  GUI.

## Adding a new type

1. Add the new type to this table with a clear firmware semantics column
   and a clear GUI control column.
2. Extend `gui/ui-schema.js` to render the control.
3. Extend the firmware Element that uses it to parse and validate.
4. Update `control_schema.json` to reference the new type.

## Possible future types (not yet used)

- `float` — floating-point config value
- `enum` — a fixed list of string options, with a `values` array
- `pin_list` — an array of `pin_id`s
