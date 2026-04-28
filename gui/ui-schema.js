// ui-schema.js — produce form controls from control_schema.json entries.
//
// See CONFIG_TYPES.md for the canonical mapping from schema type to
// control. Each builder returns a DOM node (a wa-input or a wrapped
// <select>) with the value pre-filled and a .getValue() helper attached.

// -----------------------------------------------------------------------
// Board pin map
//
// All GPIOs 1-18 are offered in the pin_id dropdown. Pins with a known
// concern (strapping role, firmware I2C, etc.) show the reason in
// parentheses so the user picks knowingly — but they're still selectable.
// If a loaded config contains a pin outside this range it's prepended as
// a "non-standard" option so the config can be viewed and reassigned.
// -----------------------------------------------------------------------

const BOARD_PINS = {
    range: [0, 23],
    // Per-GPIO concerns shown as parenthetical notes in the dropdown.
    // All pins are still selectable — these are informational only.
    //   GPIO 0  — strapping pin (boot mode; must be HIGH at power-on)
    //   GPIO 3  — strapping pin
    //   GPIO 8/9 — firmware I2C SDA/SCL (Waveshare S3-Zero wiring)
    //   GPIO 19/20 — USB D-/D+ on S3-Zero (avoid on that board)
    //   GPIO 21 — onboard WS2812 LED on Waveshare S3-Zero
    concerns: {
        0:  'strapping / boot mode',
        3:  'strapping',
        8:  'I2C SDA',
        9:  'I2C SCL',
        19: 'USB D−',
        20: 'USB D+',
        21: 'onboard LED',
    },
};

const MAX_GPIO = 48; // ESP32-S3 max; used as a permissive bound on the
                     // generic integer / milliseconds inputs.

function labelFor(entry) {
    const human = entry.id.replaceAll('_', ' ').toLowerCase();
    return human.charAt(0).toUpperCase() + human.slice(1);
}

// ----- wa-input-based number builder (used by integer / milliseconds) -----

function numberInput(entry, initial, { min, max, step = 1, suffix } = {}) {
    const input = document.createElement('wa-input');
    input.setAttribute('type', 'number');
    input.setAttribute('label', labelFor(entry));
    input.setAttribute('hint', entry.description || '');
    if (suffix) input.setAttribute('with-end', 'true');
    if (min !== undefined) input.setAttribute('min', String(min));
    if (max !== undefined) input.setAttribute('max', String(max));
    input.setAttribute('step', String(step));
    input.value = String(initial ?? entry.default ?? '');
    input.getValue = () => {
        const n = Number(input.value);
        return Number.isFinite(n) ? n : (entry.default ?? 0);
    };
    return input;
}

// ----- pin_id: native <select> dropdown, GPIO 1-18 -----
//
// All pins in the board range are offered. Pins with a concern show it
// in parentheses but are still selectable. If a loaded config contains
// a pin outside the range it's prepended as a "non-standard" option so
// the config can be viewed and reassigned without data loss.

function pinIdSelect(entry, initial) {
    const wrap = document.createElement('div');
    wrap.className = 'config-field-wrap';

    const label = document.createElement('label');
    label.className = 'config-field-label';
    label.textContent = labelFor(entry);
    if (entry.description) label.title = entry.description;
    wrap.appendChild(label);

    const select = document.createElement('select');
    select.className = 'config-field-select pin-id-select';

    const [lo, hi] = BOARD_PINS.range;
    for (let pin = lo; pin <= hi; pin++) {
        const opt = document.createElement('option');
        opt.value = String(pin);
        const concern = BOARD_PINS.concerns[pin];
        opt.textContent = concern ? `GPIO ${pin}  (${concern})` : `GPIO ${pin}`;
        select.appendChild(opt);
    }

    // Prepend an annotated option for any value outside the board range
    // so a legacy or cross-board config isn't silently mangled.
    const initN = Number(initial);
    const inRange = Number.isFinite(initN) && initN >= lo && initN <= hi;
    if (Number.isFinite(initN) && !inRange) {
        const opt = document.createElement('option');
        opt.value = String(initN);
        opt.textContent = `GPIO ${initN} (non-standard)`;
        select.insertBefore(opt, select.firstChild);
    }
    select.value = String(Number.isFinite(initN) ? initN : lo);
    wrap.appendChild(select);

    wrap.getValue = () => Number(select.value);
    // Expose for conflict detection and event wiring in app.js.
    wrap.pinSelect = select;
    wrap.isPinField = true;
    return wrap;
}

// ----- boolean: native checkbox in a labelled wrap -----

function booleanCheckbox(entry, initial) {
    const wrap = document.createElement('div');
    wrap.className = 'config-field-wrap';

    const label = document.createElement('label');
    label.className = 'config-field-label';
    label.textContent = labelFor(entry);
    if (entry.description) label.title = entry.description;
    wrap.appendChild(label);

    const row = document.createElement('div');
    row.className = 'config-field-bool-row';
    const input = document.createElement('input');
    input.type = 'checkbox';
    input.className = 'config-field-checkbox';
    // Accept JSON booleans (true/false) and the loose 0/1 / "true"/"false"
    // forms that legacy configs might leave behind.
    const initVal = (initial !== undefined) ? initial : entry.default;
    input.checked = (initVal === true || initVal === 1 || initVal === '1' || initVal === 'true');
    row.appendChild(input);

    const valLabel = document.createElement('span');
    valLabel.className = 'config-field-bool-valuelabel';
    const updateLabel = () => valLabel.textContent = input.checked ? 'on' : 'off';
    updateLabel();
    input.addEventListener('change', updateLabel);
    row.appendChild(valLabel);

    wrap.appendChild(row);
    wrap.getValue = () => input.checked; // returns a real boolean
    return wrap;
}

// ----- unknown type fallback -----

function unknownTypeBuilder(entry, initial) {
    const input = document.createElement('wa-input');
    input.setAttribute('label', `${labelFor(entry)} (unknown type: ${entry.type})`);
    input.setAttribute('hint', entry.description || '');
    input.value = String(initial ?? entry.default ?? '');
    input.getValue = () => input.value;
    return input;
}

const BUILDERS = {
    pin_id:       (entry, initial) => pinIdSelect(entry, initial),

    milliseconds: (entry, initial) =>
        numberInput(entry, initial, { min: 0, step: 1, suffix: 'ms' }),

    integer:      (entry, initial) =>
        numberInput(entry, initial, {
            min: entry.min,
            max: entry.max,
            step: 1,
        }),

    boolean:      (entry, initial) => booleanCheckbox(entry, initial),
};

// Main entry: given a config-schema entry and an initial value (or
// undefined), produce a DOM input.
export function buildControl(entry, initial) {
    const builder = BUILDERS[entry.type] || unknownTypeBuilder;
    return builder(entry, initial);
}

// Exposed for app.js so conflict checks can reason about the board
// pin set without re-declaring it.
export function getBoardPins() { return BOARD_PINS; }
