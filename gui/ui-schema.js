// ui-schema.js — produce form controls from control_schema.json entries.
//
// See CONFIG_TYPES.md for the canonical mapping from schema type to
// control. Each builder returns a DOM node (a wa-input or a wrapped
// <select>) with the value pre-filled and a .getValue() helper attached.

// -----------------------------------------------------------------------
// Board pin map
//
// The authoritative `available` / `concerns` lists live in each board's
// own `board.json`, so different breakouts (custom PCB vs bare-S3-on-
// breadboard vs all-pins-for-testing) can describe their usable GPIOs
// independently.  The GUI reads `boardData.pins` from the device and
// passes it through to `pinIdSelect` via the `ctx` argument of
// `buildControl(entry, initial, ctx)`.
//
// The fallback below is used only when `board.json` has no `pins` block
// — typical for an out-of-date board.json on a freshly-flashed device,
// or for a Central-mode peripheral whose board.json hasn't been fetched
// yet.  Keep it permissive so the dropdown always offers something
// reasonable; the user can refine the config once they update board.json.
// -----------------------------------------------------------------------

const FALLBACK_BOARD_PINS = {
    available: [1, 2, 3, 4, 7, 8, 43, 44],
    concerns: { 3: 'strapping (JTAG sel)' },
};

// Coerce concerns keys to numbers (board.json delivers them as strings
// since JSON has no numeric keys). Returns a fresh object; safe to call
// each render.
function normaliseBoardPins(raw) {
    if (!raw || !Array.isArray(raw.available)) return FALLBACK_BOARD_PINS;
    const concerns = {};
    if (raw.concerns && typeof raw.concerns === 'object') {
        for (const [k, v] of Object.entries(raw.concerns)) {
            const n = Number(k);
            if (Number.isFinite(n) && typeof v === 'string') concerns[n] = v;
        }
    }
    return { available: raw.available.slice(), concerns };
}

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

function pinIdSelect(entry, initial, ctx) {
    const boardPins = normaliseBoardPins(ctx?.boardPins);
    const wrap = document.createElement('div');
    wrap.className = 'config-field-wrap';

    const label = document.createElement('label');
    label.className = 'config-field-label';
    label.textContent = labelFor(entry);
    if (entry.description) label.title = entry.description;
    wrap.appendChild(label);

    const select = document.createElement('select');
    select.className = 'config-field-select pin-id-select';

    const available = boardPins.available;
    for (const pin of available) {
        const opt = document.createElement('option');
        opt.value = String(pin);
        const concern = boardPins.concerns[pin];
        opt.textContent = concern ? `GPIO ${pin}  (${concern})` : `GPIO ${pin}`;
        select.appendChild(opt);
    }

    // Prepend an annotated option for any value not in the available list
    // so a legacy or cross-board config isn't silently mangled.
    const initN = Number(initial);
    const inList = Number.isFinite(initN) && available.includes(initN);
    if (Number.isFinite(initN) && !inList) {
        const opt = document.createElement('option');
        opt.value = String(initN);
        opt.textContent = `GPIO ${initN} (non-standard)`;
        select.insertBefore(opt, select.firstChild);
    }
    select.value = String(Number.isFinite(initN) ? initN : available[0]);
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
    pin_id:       (entry, initial, ctx) => pinIdSelect(entry, initial, ctx),

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

// Main entry: given a config-schema entry, an initial value, and an
// optional context (currently `{ boardPins }` for pin_id dropdowns),
// produce a DOM input.
export function buildControl(entry, initial, ctx) {
    const builder = BUILDERS[entry.type] || unknownTypeBuilder;
    return builder(entry, initial, ctx);
}
