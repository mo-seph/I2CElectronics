// app.js — top-level GUI wiring.
//
// Scope v4 (M5):
//   - Role-aware: connect to either a Peripheral directly or a Central
//     that enumerates multiple Peripherals over I2C.
//   - State model is an array of `devices` — one entry per addressed
//     Peripheral. `dev.address` is null when connected directly to a
//     Peripheral, or the I2C address when routed through a Central.
//   - All commands thread `dev.address` via the Protocol helpers, which
//     prefix `@<addr>` automatically.
//   - Top bar adds a "Rescan" button that re-runs the Central's scan
//     and rebuilds the device cards.

import { SerialConnection } from './serial.js';
import { Protocol } from './protocol.js';
import { buildControl } from './ui-schema.js';

// ---------- State ----------
const state = {
    schema: null,
    typeByName: {},
    conn: null,
    protocol: null,
    role: null,        // 'peripheral' | 'central' | null
    devices: [],       // see buildDevice(); always an array
};

// ---------- DOM refs ----------
const connectBtn    = document.getElementById('connect-btn');
const disconnectBtn = document.getElementById('disconnect-btn');
const stopAllBtn    = document.getElementById('stop-all-btn');
const identifyAllBtn = document.getElementById('identify-all-btn');
const rescanBtn     = document.getElementById('rescan-btn');
const connStatus    = document.getElementById('conn-status');
const devicesEl     = document.getElementById('devices');
const logOutput     = document.getElementById('log-output');
const logClearBtn   = document.getElementById('log-clear-btn');
const cmdForm       = document.getElementById('cmd-form');
const cmdInput      = document.getElementById('cmd-input');

// ---------- Log panel ----------
function appendLog(line, kind = 'log') {
    const el = document.createElement('div');
    el.className = `log-line log-${kind}`;
    el.textContent = line;
    logOutput.appendChild(el);
    logOutput.scrollTop = logOutput.scrollHeight;
}
logClearBtn.addEventListener('click', () => { logOutput.innerHTML = ''; });

// ---------- Command input (free-form commands from user) ----------
cmdForm.addEventListener('submit', async (e) => {
    e.preventDefault();
    const cmd = cmdInput.value.trim();
    if (!cmd || !state.protocol) return;
    cmdInput.value = '';
    if (cmd === 'reboot') {
        try { await state.protocol.reboot(); appendLog('reboot sent', 'warn'); setTimeout(onDisconnect, 200); }
        catch (err) { appendLog(err.message, 'error'); }
        return;
    }
    try {
        const lines = await state.protocol.send(cmd, { timeoutMs: 8000 });
        for (const ln of lines) appendLog(ln, 'response');
        appendLog('OK', 'ok');
    } catch (err) {
        appendLog(`ERR ${err.message}`, 'error');
    }
});

// ---------- Schema ----------
async function loadSchema() {
    const r = await fetch('../shared/control_schema.json');
    if (!r.ok) throw new Error(`failed to load schema: ${r.status}`);
    state.schema = await r.json();
    state.typeByName = {};
    for (const entry of state.schema) state.typeByName[entry.type_name] = entry;
}

// ---------- Connection ----------
function setConnectionStatus(text, variant) {
    connStatus.textContent = text;
    connStatus.setAttribute('variant', variant);
    const connected = text.startsWith('connected');
    connectBtn.disabled = connected || text === 'connecting';
    disconnectBtn.disabled = !connected;
    stopAllBtn.disabled = !connected;
    identifyAllBtn.disabled = !connected;
    cmdInput.disabled = !connected;
    // Rescan button: shown only when connected to a Central.
    const isCentral = (state.role === 'central');
    rescanBtn.disabled = !(connected && isCentral);
    if (connected && isCentral) rescanBtn.removeAttribute('hidden');
    else rescanBtn.setAttribute('hidden', '');
}

async function onConnect() {
    if (!SerialConnection.supported()) {
        showError('WebSerial not supported — use Chrome, Edge, Arc or another Chromium browser.');
        return;
    }
    setConnectionStatus('connecting', 'warning');
    try {
        state.conn = new SerialConnection();
        state.protocol = new Protocol(state.conn, {
            onLog:  line => appendLog(line, 'log'),
            onSend: cmd  => appendLog(`→ ${cmd}`, 'sent'),
        });
        state.conn.onClose(onConnectionClosed);
        await state.conn.connect();
        // role still unknown until we do whoami
        setConnectionStatus('connected', 'success');
        setTimeout(queryDevices, 200);
    } catch (err) {
        appendLog(`connect error: ${err.message}`, 'error');
        showError(err.message);
        setConnectionStatus('disconnected', 'neutral');
        state.conn = null;
        state.protocol = null;
    }
}

async function onDisconnect() {
    if (state.protocol) { state.protocol.close(); state.protocol = null; }
    if (state.conn) { await state.conn.disconnect().catch(() => {}); state.conn = null; }
    state.role = null;
    state.devices = [];
    renderDevices();
    setConnectionStatus('disconnected', 'neutral');
}

function onConnectionClosed() {
    appendLog('(serial port closed)', 'error');
    state.role = null;
    state.devices = [];
    renderDevices();
    setConnectionStatus('disconnected', 'neutral');
}

// ---------- Query / populate devices ----------

// Fetch the role via whoami. If central, enumerate peripherals and
// build a synthetic entry per address (no full cfg/board read — deferred
// until the user opens a config UI, see ensureConfigLoaded). If
// peripheral (direct), fetch eagerly — it's fast on a local link.
async function queryDevices() {
    try {
        const who = await state.protocol.whoami();
        state.role = who.role;
        if (who.role === 'peripheral') {
            const dev = await buildDeviceEager(null);
            state.devices = dev ? [dev] : [];
            setConnectionStatus(`connected — peripheral`, 'success');
        } else if (who.role === 'central') {
            const peripherals = await state.protocol.listPeripherals();
            state.devices = peripherals.map(p => buildDeviceSynthetic(p));
            const online = state.devices.length;
            setConnectionStatus(`connected — central (${online} peripheral${online === 1 ? '' : 's'})`, 'success');
        } else {
            appendLog(`unknown role "${who.role}"`, 'error');
            state.devices = [];
        }
        renderDevices();
    } catch (err) {
        appendLog(`query error: ${err.message}`, 'error');
        showError(`Failed to query device: ${err.message}`);
    }
}

// Shared device skeleton.
function makeDeviceShell(address, listInfo) {
    return {
        address,                 // null for direct, number for central->peripheral
        listInfo,                // from Central's `list` — or null
        boardData: null,
        configData: null,
        liveElements: [],
        configLoaded: false,     // have we actually fetched board+config yet?
        _cardEl: null,
        _summaryRowsEl: null,
        _boardDemoSwitch: null,
        _configHidden: false,
    };
}

// Build a device with real board+config fetched over the tunnel/serial.
// Used for direct peripheral connections (fast) and for on-demand
// refreshes in central mode.
async function buildDeviceEager(address, listInfo = null) {
    const dev = makeDeviceShell(address, listInfo);
    try { dev.boardData  = await state.protocol.readBoard(address); }
    catch (err) { appendLog(`read board for ${address ?? 'device'} failed: ${err.message}`, 'error'); }
    try { dev.configData = await state.protocol.readConfig(address); }
    catch (err) { appendLog(`read config for ${address ?? 'device'} failed: ${err.message}`, 'error'); }
    dev.configLoaded = true;
    return dev;
}

// Build a device with just the type_ids from Central's `list` output —
// no board.json or per-element config values yet. Enough to render
// sliders / demo / stop controls for the hot path; fields are populated
// on demand via ensureConfigLoaded when the user opens a Config UI.
function buildDeviceSynthetic(listInfo) {
    const dev = makeDeviceShell(listInfo.address, listInfo);
    dev.configData = typeIdsToSyntheticConfig(listInfo.typeIds);
    return dev;
}

function typeIdsToSyntheticConfig(typeIds) {
    return typeIds.map((tid, i) => {
        const typeEntry = (state.schema || []).find(t => t.type_id === tid);
        const typeName = typeEntry?.type_name || `unknown(${tid})`;
        return { index: i + 1, type: typeName, config: {} };
    });
}

// On-demand fetch of the full config for a device. Re-renders the card
// in place once loaded. Returns true on success, false on failure.
async function ensureConfigLoaded(dev) {
    if (dev.configLoaded) return true;
    appendLog(`loading config for @${dev.address}…`, 'log');
    try {
        const bd = await state.protocol.readBoard(dev.address);
        const cd = await state.protocol.readConfig(dev.address);
        dev.boardData = bd;
        dev.configData = cd;
        dev.configLoaded = true;

        // Re-render this card in place. Summary slider values reset to 0
        // by the rebuild — a minor wart; the user can re-set from the UI.
        //
        // Capture the OLD card before renderPeripheralCard mutates
        // dev._cardEl to the new one (it assigns inside). Without this,
        // replaceChild silently no-ops (the "old card" and "new card"
        // are the same ref; newCard has no parentNode yet).
        const oldCard = dev._cardEl;
        const newCard = renderPeripheralCard(dev);
        if (oldCard && oldCard.parentNode) {
            oldCard.parentNode.replaceChild(newCard, oldCard);
        }
        return true;
    } catch (err) {
        appendLog(`load config for @${dev.address} failed: ${err.message}`, 'error');
        return false;
    }
}

// Full re-enumerate for Central. Preserves loaded-config state for
// peripherals that are still present at the same address.
async function rescan() {
    if (state.role !== 'central' || !state.protocol) return;
    try {
        await state.protocol.scan();
        appendLog('scan complete — re-enumerating', 'ok');
        const peripherals = await state.protocol.listPeripherals();
        const prevByAddr = Object.fromEntries(
            state.devices.map(d => [d.address, d])
        );
        state.devices = peripherals.map(p => {
            const prev = prevByAddr[p.address];
            if (prev && prev.configLoaded) {
                // Keep the already-loaded device; just refresh listInfo.
                prev.listInfo = p;
                return prev;
            }
            return buildDeviceSynthetic(p);
        });
        const online = state.devices.length;
        setConnectionStatus(`connected — central (${online} peripheral${online === 1 ? '' : 's'})`, 'success');
        renderDevices();
    } catch (err) {
        appendLog(`rescan failed: ${err.message}`, 'error');
    }
}

// ---------- Top-bar event wiring ----------
connectBtn.addEventListener('click', onConnect);
disconnectBtn.addEventListener('click', onDisconnect);
stopAllBtn.addEventListener('click', async () => {
    if (!state.protocol) return;
    try {
        const cmd = (state.role === 'central') ? '@all stop' : 'stop';
        await state.protocol.send(cmd);
        appendLog('stopped all', 'ok');
        // Reset per-element UI state for every device we know about.
        for (const dev of state.devices) {
            if (dev._boardDemoSwitch) dev._boardDemoSwitch.checked = false;
            for (const rec of dev.liveElements) {
                if (rec.demoSwitch) rec.demoSwitch.checked = false;
                for (const p of rec.params) {
                    p.value = 0;
                    syncParamUI(p);
                }
            }
        }
    } catch (err) { appendLog(`stop failed: ${err.message}`, 'error'); }
});
rescanBtn.addEventListener('click', rescan);

// Identify-all: turn every known device's LED off for 10s so a crashed
// or stuck board stands out against the sea of dark ones. In Central
// mode fans out to every peripheral via the text tunnel; in direct
// mode just talks to the one connected device.
identifyAllBtn.addEventListener('click', async () => {
    if (!state.protocol) return;
    const seconds = 10;
    try {
        if (state.role === 'central') {
            // Also turn off the Central's own LED.
            await state.protocol.send(`ledoff ${seconds}`);
            // Fan out to each peripheral via tunnel — sequential, but
            // each takes ~50 ms so typical networks finish well under
            // the ledoff window.
            for (const dev of state.devices) {
                try { await state.protocol.send(`@${dev.address} ledoff ${seconds}`, { timeoutMs: 5000 }); }
                catch (err) { appendLog(`ledoff @${dev.address} failed: ${err.message}`, 'error'); }
            }
            appendLog(`all LEDs off for ${seconds}s — frozen boards will stay lit`, 'ok');
        } else {
            await state.protocol.send(`ledoff ${seconds}`);
            appendLog(`LED off for ${seconds}s`, 'ok');
        }
    } catch (err) { appendLog(`all LEDs off failed: ${err.message}`, 'error'); }
});

function showError(msg) { appendLog(`ERROR: ${msg}`, 'error'); }

// ---------- Rendering ----------
function renderDevices() {
    devicesEl.innerHTML = '';
    if (!state.role) {
        devicesEl.innerHTML = `
            <div class="empty-hint">
                No device connected.<br>
                <small>Connect via USB and click "Connect" to begin. Requires a Chromium-based browser (Chrome, Edge, Arc, Brave) over HTTPS or localhost.</small>
            </div>`;
        return;
    }
    if (state.role === 'central' && state.devices.length === 0) {
        devicesEl.innerHTML = `
            <div class="empty-hint">
                Central connected — no peripherals found.<br>
                <small>Wire up a Peripheral and click <strong>Rescan</strong>.</small>
            </div>`;
        return;
    }
    for (const dev of state.devices) {
        devicesEl.appendChild(renderPeripheralCard(dev));
    }
}

function btn(label, attrs = {}, onClick) {
    const b = document.createElement('wa-button');
    for (const [k, v] of Object.entries(attrs)) b.setAttribute(k, v);
    b.textContent = label;
    if (onClick) b.addEventListener('click', onClick);
    return b;
}

function formatI2CAddress(v) {
    if (v === undefined || v === null) return '(unknown)';
    const n = Number(v);
    return `${n} (0x${n.toString(16).padStart(2, '0').toUpperCase()})`;
}

// ---------- Peripheral card ----------
function renderPeripheralCard(dev) {
    const card = document.createElement('wa-card');
    card.className = 'device-card';
    dev._cardEl = card;

    const addrLabel = (dev.address == null) ? 'direct' : `@${dev.address}`;

    // Show i2c address from boardData if we have it; otherwise fall back
    // to the listInfo address (which for Central-mode is the same address
    // we're routing through) so there's always something useful shown.
    const i2cShown = dev.boardData?.i2c_address ?? dev.address;
    const loadedHint = dev.configLoaded
        ? ''
        : ' <span class="subtle">· config not loaded — click Config to edit</span>';

    const header = document.createElement('div');
    header.slot = 'header';
    header.className = 'device-header';
    header.innerHTML = `
        <div>
            <strong>Peripheral <span class="subtle">(${addrLabel})</span></strong>
            <div class="subtle">i2c: ${formatI2CAddress(i2cShown)}${dev.listInfo?.stale ? ' <span class="error-text">stale</span>' : ''}${loadedHint}</div>
        </div>
    `;

    const actions = document.createElement('div');
    actions.className = 'device-header-actions';

    // Board-level Demo switch — sends `demo all on|off` to this peripheral
    // only (whether addressed or direct).
    const boardDemoSwitch = document.createElement('wa-switch');
    boardDemoSwitch.setAttribute('size', 'small');
    boardDemoSwitch.textContent = 'Demo';
    boardDemoSwitch.addEventListener('change', async () => {
        const on = boardDemoSwitch.checked;
        try {
            await state.protocol.send(devCmd(dev, `demo all ${on ? 'on' : 'off'}`));
            for (const rec of dev.liveElements) {
                if (rec.demoSwitch) rec.demoSwitch.checked = on;
            }
        } catch (err) {
            appendLog(`demo all failed: ${err.message}`, 'error');
            boardDemoSwitch.checked = !on;
        }
    });
    dev._boardDemoSwitch = boardDemoSwitch;
    actions.appendChild(boardDemoSwitch);

    const boardStopBtn = btn('Stop', { size: 'small', variant: 'danger' }, async () => {
        try {
            await state.protocol.send(devCmd(dev, 'stop'));
            for (const rec of dev.liveElements) {
                if (rec.demoSwitch) rec.demoSwitch.checked = false;
                for (const p of rec.params) {
                    p.value = 0;
                    syncParamUI(p);
                }
            }
            boardDemoSwitch.checked = false;
        } catch (err) { appendLog(`stop failed: ${err.message}`, 'error'); }
    });
    actions.appendChild(boardStopBtn);

    // Blink identifiers — quick (2 s) for at-a-glance check, longer
    // (10 s) when you need time to physically find the board. Both
    // route identically through the protocol layer; direct peripheral
    // takes the plain command path, addressed takes the text tunnel.
    const blink2sBtn = btn('Blink 2s', { size: 'small', appearance: 'outlined' }, async () => {
        try { await state.protocol.send(devCmd(dev, 'blink 2'), { timeoutMs: 4000 }); }
        catch (err) { appendLog(`blink failed: ${err.message}`, 'error'); }
    });
    const blink10sBtn = btn('Blink 10s', { size: 'small', appearance: 'outlined' }, async () => {
        try { await state.protocol.send(devCmd(dev, 'blink 10'), { timeoutMs: 4000 }); }
        catch (err) { appendLog(`blink failed: ${err.message}`, 'error'); }
    });
    actions.append(blink2sBtn, blink10sBtn);

    // Peripheral-level Config toggle: compact vs expanded.
    // When config hasn't been fetched yet (Central mode), the first click
    // triggers a load and re-render — the new card will be in expanded
    // state, which is the natural result the user wanted anyway.
    const configToggleBtn = btn('Config', { size: 'small',
            appearance: dev._configHidden || !dev.configLoaded ? 'outlined' : 'filled' }, async () => {
        if (!dev.configLoaded) {
            dev._configHidden = false; // show after the load completes
            await ensureConfigLoaded(dev);
            return; // ensureConfigLoaded re-renders the card
        }
        dev._configHidden = !dev._configHidden;
        card.classList.toggle('compact', dev._configHidden);
        configToggleBtn.setAttribute('appearance', dev._configHidden ? 'outlined' : 'filled');
    });
    if (dev._configHidden) card.classList.add('compact');
    actions.appendChild(configToggleBtn);

    const reloadBtn = btn('Reload', { size: 'small', appearance: 'outlined' }, async () => {
        // Force a fresh fetch even if already loaded.
        dev.configLoaded = false;
        const ok = await ensureConfigLoaded(dev);
        if (ok) appendLog(`reloaded ${addrLabel}`, 'ok');
    });
    const rebootBtn = btn('Reboot', { size: 'small', variant: 'warning' }, async () => {
        try {
            await state.protocol.reboot(dev.address);
            appendLog(`reboot sent to ${addrLabel}`, 'warn');
            if (dev.address == null) {
                setTimeout(onDisconnect, 200);
            } else {
                // Peripheral-through-Central reboot: wait then rescan.
                setTimeout(rescan, 1500);
            }
        } catch (err) { showError(err.message); }
    });
    actions.append(reloadBtn, rebootBtn);
    header.appendChild(actions);
    card.appendChild(header);

    dev._summarySection = renderSummarySection(dev);
    card.appendChild(dev._summarySection);
    card.appendChild(renderBoardSection(dev));
    card.appendChild(renderElementsSection(dev));

    rebuildSummaryRows(dev);
    return card;
}

// Turn a device-relative command (e.g. "demo 1 on") into the right wire
// form by prepending @<addr> if needed.
function devCmd(dev, cmd) {
    return (dev.address == null) ? cmd : `@${dev.address} ${cmd}`;
}

// ---------- Summary section (shown only in compact view) ----------
function renderSummarySection(dev) {
    const section = document.createElement('section');
    section.className = 'summary-section';
    const h = document.createElement('div');
    h.className = 'section-header';
    h.textContent = 'Parameters';
    section.appendChild(h);
    const rows = document.createElement('div');
    rows.className = 'summary-rows';
    section.appendChild(rows);
    dev._summaryRowsEl = rows;
    return section;
}

function rebuildSummaryRows(dev) {
    const container = dev._summaryRowsEl;
    if (!container) return;
    container.innerHTML = '';
    for (const rec of dev.liveElements) {
        if (!rec.typeEntry?.parameters) continue;
        rec.typeEntry.parameters.forEach((name, i) => {
            const row = document.createElement('div');
            row.className = 'param-row';

            const labelEl = document.createElement('label');
            labelEl.className = 'param-label';
            labelEl.innerHTML = `<span class="idx">#${rec.dataEntry.index}</span> ${name}`;

            const slider = document.createElement('wa-slider');
            slider.setAttribute('min', '0');
            slider.setAttribute('max', '1');
            slider.setAttribute('step', '0.01');
            slider.setAttribute('with-tooltip', '');
            slider.value = rec.params[i].value;

            const valEl = document.createElement('span');
            valEl.className = 'param-val';
            valEl.textContent = rec.params[i].value.toFixed(2);

            slider.addEventListener('input', () => {
                const v = Number(slider.value);
                const p = rec.params[i];
                p.value = v;
                if (p.elSlider) p.elSlider.value = v;
                if (p.elValEl) p.elValEl.textContent = v.toFixed(2);
                valEl.textContent = v.toFixed(2);
                sendParam(rec, i, v, dev);
            });

            rec.params[i].sumSlider = slider;
            rec.params[i].sumValEl  = valEl;

            row.append(labelEl, slider, valEl);
            container.appendChild(row);
        });
    }
    if (!container.children.length) {
        const empty = document.createElement('div');
        empty.className = 'subtle';
        empty.textContent = '(no parameters)';
        container.appendChild(empty);
    }
}

// ---------- Board section ----------
function renderBoardSection(dev) {
    const section = document.createElement('section');
    section.className = 'board-section config-block';

    const h = document.createElement('h3');
    h.textContent = 'Board';
    section.appendChild(h);

    const addrInput = document.createElement('wa-input');
    addrInput.setAttribute('type', 'number');
    addrInput.setAttribute('label', 'I2C address');
    addrInput.setAttribute('min', '1');
    addrInput.setAttribute('max', '127');
    addrInput.setAttribute('hint', 'Decimal (0x10 = 16). Must be unique on the I2C bus.');
    addrInput.value = String(dev.boardData?.i2c_address ?? 16);
    section.appendChild(addrInput);

    const saveBtn = btn('Save board', { variant: 'brand', size: 'small' }, async () => {
        // If config hasn't been fetched yet, load it first so the user
        // sees the real values before overwriting them with form defaults.
        if (!dev.configLoaded) {
            const ok = await ensureConfigLoaded(dev);
            if (!ok) return;
            appendLog('board loaded — review and click Save board again to write', 'warn');
            return;
        }
        const next = { i2c_address: Number(addrInput.value) };
        try {
            await state.protocol.writeBoard(next, dev.address);
            dev.boardData = next;
            appendLog(`board saved: ${JSON.stringify(next)}`, 'ok');
            notifyRebootNeeded(dev);
        } catch (err) { showError(`Save board failed: ${err.message}`); }
    });
    saveBtn.style.marginTop = '0.75em';
    section.appendChild(saveBtn);
    return section;
}

// ---------- Elements section ----------
function renderElementsSection(dev) {
    const section = document.createElement('section');
    section.className = 'elements-section';

    const header = document.createElement('div');
    header.className = 'section-header';
    const h = document.createElement('span');
    h.textContent = 'Elements';
    const addBtn = btn('+ Add', { size: 'small' },
        () => openAddElementDialog(dev, section));
    header.append(h, addBtn);
    section.appendChild(header);

    const list = document.createElement('div');
    list.className = 'elements-list';
    section.appendChild(list);

    dev.liveElements = (dev.configData || []).map(entry => {
        const rec = buildElementCard(entry, dev);
        list.appendChild(rec.cardEl);
        return rec;
    });

    const saveBtn     = btn('Save elements', { variant: 'brand', size: 'small' },
        () => saveElements(dev));
    const saveFileBtn = btn('↓ Save file', { size: 'small', appearance: 'outlined' },
        () => saveConfigToFile(dev));
    const loadFileBtn = btn('↑ Load file', { size: 'small', appearance: 'outlined' },
        () => loadConfigFromFile(dev));

    const btnRow = document.createElement('div');
    btnRow.style.cssText = 'display:flex;gap:0.4rem;margin-top:0.75em;flex-wrap:wrap;';
    btnRow.append(saveBtn, saveFileBtn, loadFileBtn);
    section.appendChild(btnRow);

    // First-paint conflict check after all cards are in place.
    checkPinConflicts(dev);

    return section;
}

// ---------- Element card ----------
function buildElementCard(entry, dev, { configVisible = false } = {}) {
    const typeEntry = state.typeByName[entry.type];
    const rec = {
        dataEntry: entry,
        typeEntry,
        fieldInputs: [],
        params: [],
        demoSwitch: null,
        configBlockEl: null,
        configToggleBtn: null,
        cardEl: null,
    };

    const cardEl = document.createElement('wa-card');
    cardEl.className = 'element-card';
    rec.cardEl = cardEl;

    const header = document.createElement('div');
    header.className = 'element-header';
    const titleWrap = document.createElement('div');
    titleWrap.innerHTML = `<strong>#${entry.index} · ${entry.type}</strong>`;
    header.appendChild(titleWrap);

    const hdrActions = document.createElement('div');
    hdrActions.className = 'element-header-actions';

    const demoSwitch = document.createElement('wa-switch');
    demoSwitch.setAttribute('size', 'small');
    demoSwitch.textContent = 'Demo';
    demoSwitch.addEventListener('change', async () => {
        if (!state.protocol) return;
        const on = demoSwitch.checked;
        try {
            await state.protocol.send(devCmd(dev, `demo ${entry.index} ${on ? 'on' : 'off'}`));
        } catch (err) {
            appendLog(`demo ${entry.index} failed: ${err.message}`, 'error');
            demoSwitch.checked = !on;
        }
    });
    rec.demoSwitch = demoSwitch;
    hdrActions.appendChild(demoSwitch);

    const stopBtn = btn('Stop', { size: 'small', variant: 'danger', appearance: 'outlined' }, async () => {
        if (!state.protocol) return;
        try {
            // Per-element stop rides the text tunnel (stop <idx>), which
            // for a direct peripheral is just a plain command.
            await state.protocol.send(devCmd(dev, `stop ${entry.index}`), { timeoutMs: 4000 });
            if (demoSwitch.checked) demoSwitch.checked = false;
            for (const p of rec.params) {
                p.value = 0;
                syncParamUI(p);
            }
        } catch (err) {
            appendLog(`stop ${entry.index} failed: ${err.message}`, 'error');
        }
    });
    hdrActions.appendChild(stopBtn);

    const configToggleBtn = btn('Config', { size: 'small', appearance: 'outlined' }, async () => {
        // Central mode: the very first click loads real config via the
        // tunnel; the re-rendered card then shows the config section
        // expanded by default.
        if (!dev.configLoaded) {
            await ensureConfigLoaded(dev);
            return;
        }
        if (!rec.configBlockEl) return;
        const nowHidden = rec.configBlockEl.classList.toggle('hidden');
        rec.configToggleBtn.setAttribute('appearance', nowHidden ? 'outlined' : 'filled');
    });
    rec.configToggleBtn = configToggleBtn;
    hdrActions.appendChild(configToggleBtn);

    const removeBtn = btn('×', { size: 'small', appearance: 'plain', variant: 'danger' }, () => {
        dev.liveElements = dev.liveElements.filter(r => r !== rec);
        cardEl.remove();
        rebuildSummaryRows(dev);
        checkPinConflicts(dev);
    });
    hdrActions.appendChild(removeBtn);
    header.appendChild(hdrActions);
    cardEl.appendChild(header);

    // Element-local parameter sliders.
    if (typeEntry?.parameters?.length) {
        const paramsBlock = document.createElement('div');
        paramsBlock.className = 'element-params';
        typeEntry.parameters.forEach((name, i) => {
            const row = document.createElement('div');
            row.className = 'param-row';

            const labelEl = document.createElement('label');
            labelEl.className = 'param-label';
            labelEl.textContent = name;

            const slider = document.createElement('wa-slider');
            slider.setAttribute('min', '0');
            slider.setAttribute('max', '1');
            slider.setAttribute('step', '0.01');
            slider.setAttribute('with-tooltip', '');
            slider.value = 0;

            const valEl = document.createElement('span');
            valEl.className = 'param-val';
            valEl.textContent = '0.00';

            const p = {
                value: 0,
                elSlider: slider,
                elValEl: valEl,
                sumSlider: null,
                sumValEl: null,
                sendState: { inFlight: false, pending: null },
            };
            rec.params.push(p);

            slider.addEventListener('input', () => {
                const v = Number(slider.value);
                p.value = v;
                if (p.sumSlider) p.sumSlider.value = v;
                if (p.sumValEl)  p.sumValEl.textContent = v.toFixed(2);
                valEl.textContent = v.toFixed(2);
                sendParam(rec, i, v, dev);
            });

            row.append(labelEl, slider, valEl);
            paramsBlock.appendChild(row);
        });
        cardEl.appendChild(paramsBlock);
    }

    // Config block.
    if (typeEntry) {
        const configBlock = document.createElement('div');
        configBlock.className = 'element-config config-block';
        if (!configVisible) configBlock.classList.add('hidden');
        rec.configBlockEl = configBlock;
        if (configVisible) rec.configToggleBtn.setAttribute('appearance', 'filled');

        const fieldGrid = document.createElement('div');
        fieldGrid.className = 'config-field-grid';
        const ctx = { boardPins: dev.boardData?.pins };
        for (const cfgEntry of typeEntry.config) {
            const initial = entry.config?.[cfgEntry.id];
            const input = buildControl(cfgEntry, initial, ctx);
            input.classList.add('compact');
            fieldGrid.appendChild(input);
            rec.fieldInputs.push({ cfgEntry, input });
            // Wire pin-conflict detection — re-run across this device
            // whenever any pin_id select changes.
            if (input.isPinField && input.pinSelect) {
                input.pinSelect.addEventListener('change', () => checkPinConflicts(dev));
            }
        }
        configBlock.appendChild(fieldGrid);
        cardEl.appendChild(configBlock);
    } else {
        const warn = document.createElement('wa-callout');
        warn.setAttribute('variant', 'warning');
        warn.textContent = `Type "${entry.type}" is not in control_schema.json.`;
        cardEl.appendChild(warn);
    }

    return rec;
}

// ---------- Pin conflict detection ----------
//
// Walk all pin_id fields across all Elements on `dev`, flag any GPIO
// number that's used by more than one field. Re-run whenever a pin
// select changes, on add, on remove.
//
// Marks at two levels:
//   - the offending `.config-field-wrap.pin-conflict` (red border + "pin
//     used by another element" tooltip), visible when the config block
//     is expanded.
//   - the enclosing `.element-card.has-pin-conflict`, so the conflict
//     stays visible (red ribbon on the card header) even when the
//     config block is collapsed.
function checkPinConflicts(dev) {
    // Collect pin wraps and the element-card each wrap lives in, in lockstep.
    const pinWraps = [];
    const cardOf   = []; // card containing pinWraps[i]
    for (const rec of dev.liveElements) {
        for (const f of rec.fieldInputs) {
            if (f.input?.isPinField) {
                pinWraps.push(f.input);
                cardOf.push(rec.cardEl);
            }
        }
    }
    const countByPin = new Map();
    for (const w of pinWraps) {
        const pin = w.getValue();
        countByPin.set(pin, (countByPin.get(pin) || 0) + 1);
    }
    // Clear card-level flags first; we'll re-set below.
    for (const rec of dev.liveElements) rec.cardEl?.classList.remove('has-pin-conflict');
    for (let i = 0; i < pinWraps.length; i++) {
        const w = pinWraps[i];
        const conflict = countByPin.get(w.getValue()) > 1;
        w.classList.toggle('pin-conflict', conflict);
        if (conflict) cardOf[i]?.classList.add('has-pin-conflict');
    }
}

// ---------- Param UI sync ----------
function syncParamUI(p) {
    if (p.elSlider)   p.elSlider.value   = p.value;
    if (p.elValEl)    p.elValEl.textContent  = p.value.toFixed(2);
    if (p.sumSlider)  p.sumSlider.value  = p.value;
    if (p.sumValEl)   p.sumValEl.textContent = p.value.toFixed(2);
}

// ---------- Parameter send (pi, per-param throttled) ----------
function sendParam(rec, i, value, dev) {
    if (!state.protocol) return;
    const p = rec.params[i];
    const s = p.sendState;
    if (s.inFlight) { s.pending = value; return; }
    s.inFlight = true;
    const cmd = devCmd(dev, `pi ${rec.dataEntry.index} ${i} ${value.toFixed(3)}`);
    state.protocol.send(cmd, { timeoutMs: 2000 })
        .catch(err => appendLog(`pi err: ${err.message}`, 'error'))
        .finally(() => {
            s.inFlight = false;
            if (s.pending !== null) {
                const next = s.pending;
                s.pending = null;
                sendParam(rec, i, next, dev);
            }
        });
}

// ---------- Save / Add / Remove ----------
async function saveElements(dev) {
    // Don't let the user blow away real config with synthetic blanks.
    // First click on Save while unloaded just triggers the load; they
    // click again to actually write.
    if (!dev.configLoaded) {
        const ok = await ensureConfigLoaded(dev);
        if (!ok) return;
        appendLog('config loaded — review values and click Save elements again to write', 'warn');
        return;
    }
    const next = dev.liveElements.map((rec, i) => {
        const cfg = {};
        for (const f of rec.fieldInputs) cfg[f.cfgEntry.id] = f.input.getValue();
        return { index: i + 1, type: rec.dataEntry.type, config: cfg };
    });
    // Diagnostic: surface exactly what we're about to send so pin / multi-
    // element issues are easy to pin down from the log panel.
    appendLog(`saving config: ${JSON.stringify(next)}`, 'log');
    try {
        await state.protocol.writeConfig(next, dev.address);
        dev.configData = next;
        appendLog(`config saved (${next.length} element(s))`, 'ok');
        notifyRebootNeeded(dev);
    } catch (err) {
        showError(`Save config failed: ${err.message}`);
    }
}

// Add Element dialog: plain native <dialog> + native <select> + native
// <button>. No Web Awesome components — they add a "popover inside a
// popover" interaction that broke this flow in hard-to-debug ways.
// Native HTML has fine defaults (modal, ESC to close, backdrop).
function openAddElementDialog(dev, section) {
    const dlg = document.createElement('dialog');
    dlg.className = 'simple-dialog';

    const form = document.createElement('form');
    form.method = 'dialog'; // submit closes the dialog with returnValue

    const title = document.createElement('h3');
    title.textContent = 'Add Element';
    form.appendChild(title);

    const row = document.createElement('div');
    row.className = 'simple-dialog-row';

    const label = document.createElement('label');
    label.textContent = 'Type';
    label.htmlFor = 'add-element-type';

    const select = document.createElement('select');
    select.id = 'add-element-type';
    for (const t of state.schema) {
        const opt = document.createElement('option');
        opt.value = t.type_name;
        opt.textContent = t.type_name;
        select.appendChild(opt);
    }
    row.append(label, select);
    form.appendChild(row);

    const actions = document.createElement('div');
    actions.className = 'simple-dialog-actions';
    const cancelBtn = document.createElement('button');
    cancelBtn.type = 'button';
    cancelBtn.textContent = 'Cancel';
    cancelBtn.addEventListener('click', () => dlg.close('cancel'));
    const addBtn = document.createElement('button');
    addBtn.type = 'submit';
    addBtn.value = 'add';
    addBtn.textContent = 'Add';
    actions.append(cancelBtn, addBtn);
    form.appendChild(actions);

    dlg.appendChild(form);
    document.body.appendChild(dlg);

    dlg.addEventListener('close', () => {
        // Step-by-step logs so a silent failure surfaces in DevTools.
        console.log('[add-element] close event, returnValue =', dlg.returnValue);
        try {
            if (dlg.returnValue !== 'add') return;

            const typeName = select.value;
            console.log('[add-element] typeName =', typeName);
            const typeEntry = state.typeByName[typeName];
            if (!typeEntry) {
                appendLog(`add element: no type selected (value was "${typeName}")`, 'error');
                return;
            }
            const newIndex = dev.liveElements.length + 1;
            console.log('[add-element] newIndex =', newIndex,
                        'liveElements.length (before) =', dev.liveElements.length);

            const defaults = {};
            for (const cfgEntry of typeEntry.config) {
                if (cfgEntry.default !== undefined) defaults[cfgEntry.id] = cfgEntry.default;
            }
            const newEntry = { index: newIndex, type: typeName, config: defaults };

            const rec = buildElementCard(newEntry, dev, { configVisible: true });
            console.log('[add-element] buildElementCard returned rec:', rec,
                        'cardEl:', rec?.cardEl);
            if (!rec?.cardEl) {
                appendLog(`add element: buildElementCard returned no cardEl`, 'error');
                return;
            }

            // Prefer the LIVE elements-list (via dev._cardEl, which always
            // points at the current card). Fall back to the captured
            // `section` closure if the live card isn't queryable for some
            // reason. This protects against `section` being detached from
            // the document after a Reload / ensureConfigLoaded re-render.
            const liveList = dev._cardEl?.querySelector('.elements-list');
            const fallbackList = section.querySelector('.elements-list');
            const list = liveList || fallbackList;
            console.log('[add-element] list candidates: live=', liveList,
                        'fallback=', fallbackList,
                        'section.isConnected=', section.isConnected,
                        'dev._cardEl.isConnected=', dev._cardEl?.isConnected);
            if (!list) {
                appendLog(`add element: elements-list not found anywhere (isConnected: section=${section.isConnected}, card=${dev._cardEl?.isConnected})`, 'error');
                return;
            }
            if (!list.isConnected) {
                appendLog(`add element: elements-list is detached from document (stale section/card)`, 'error');
                return;
            }

            list.appendChild(rec.cardEl);
            console.log('[add-element] appended; list.children.length =', list.children.length,
                        'rec.cardEl.isConnected =', rec.cardEl.isConnected);

            dev.liveElements.push(rec);
            console.log('[add-element] liveElements.length (after) =', dev.liveElements.length);

            rebuildSummaryRows(dev);
            checkPinConflicts(dev);
            appendLog(`added element #${newIndex} ${typeName} (click 'Save elements' to persist)`, 'log');
        } catch (err) {
            appendLog(`add element threw: ${err.message}`, 'error');
            console.error('[add-element] exception:', err);
        } finally {
            // Always clean up the dialog node — even on early-return paths.
            dlg.remove();
        }
    });

    dlg.showModal();
}

function notifyRebootNeeded(dev) {
    const who = (dev.address == null) ? 'device' : `peripheral @${dev.address}`;
    appendLog(`Saved to ${who}. Reboot to apply.`, 'warn');
}

// ---------- Save / Load config file ----------

// Download the current element config as a JSON file. Reads from the live
// form state (same values that "Save elements" would push), so what you
// download reflects what the UI currently shows.
function saveConfigToFile(dev) {
    if (!dev.configLoaded) {
        appendLog('config not loaded yet — click Config to load it first, then Save file', 'warn');
        return;
    }
    const data = dev.liveElements.map((rec, i) => {
        const cfg = {};
        for (const f of rec.fieldInputs) cfg[f.cfgEntry.id] = f.input.getValue();
        return { index: i + 1, type: rec.dataEntry.type, config: cfg };
    });
    const json = JSON.stringify(data, null, 2);
    const blob = new Blob([json], { type: 'application/json' });
    const url  = URL.createObjectURL(blob);
    const a    = document.createElement('a');
    a.href     = url;
    const addrStr = (dev.address == null) ? 'peripheral' : `addr${dev.address}`;
    a.download = `ao-config-${addrStr}.json`;
    a.click();
    URL.revokeObjectURL(url);
    appendLog(`config saved to file (${data.length} element(s))`, 'ok');
}

// Load a config JSON file and push it to the device, then reload the card
// so the UI matches. The file must be an array of element objects with at
// least `type` and `config` fields (the same format that Save file produces
// and that the presets/ directory contains).
async function loadConfigFromFile(dev) {
    if (!state.protocol) { appendLog('not connected', 'error'); return; }

    const input = document.createElement('input');
    input.type   = 'file';
    input.accept = '.json,application/json';

    input.addEventListener('change', async () => {
        const file = input.files?.[0];
        if (!file) return;
        let data;
        try {
            const text = await file.text();
            data = JSON.parse(text);
        } catch (err) {
            appendLog(`load file: JSON parse failed — ${err.message}`, 'error');
            return;
        }
        if (!Array.isArray(data) || data.length === 0) {
            appendLog('load file: file must contain a non-empty JSON array of elements', 'error');
            return;
        }
        if (!data.every(e => typeof e.type === 'string' && typeof e.config === 'object')) {
            appendLog('load file: each element needs "type" (string) and "config" (object)', 'error');
            return;
        }
        // Re-index so indices are always 1, 2, 3... regardless of what the
        // file stored (avoids confusion if someone hand-edits a preset).
        const normalised = data.map((e, i) => ({ ...e, index: i + 1 }));
        appendLog(`loading ${normalised.length} element(s) from ${file.name}…`, 'log');
        try {
            await state.protocol.writeConfig(normalised, dev.address);
            appendLog(`config pushed to device — reloading…`, 'ok');
            notifyRebootNeeded(dev);
        } catch (err) {
            appendLog(`load file: write failed — ${err.message}`, 'error');
            return;
        }
        // Reload from device so the UI reflects what was actually stored.
        dev.configLoaded = false;
        await ensureConfigLoaded(dev);
    });

    input.click();
}

// ---------- Bootstrap ----------
(async () => {
    try {
        await loadSchema();
        setConnectionStatus('disconnected', 'neutral');
    } catch (err) {
        showError(`Schema load failed: ${err.message}`);
    }
})();
