// protocol.js — framing layer over SerialConnection.
//
// Implements the text protocol documented in shared/serial_protocol.md:
//   - One command in flight at a time.
//   - Data lines accumulate until `OK` (success) or `ERR <msg>` (failure).
//   - Lines beginning with `# ` are logs — forwarded to onLog, never part
//     of a response.
//   - Lines starting with `> ` (prompt) have that prefix stripped before
//     further processing, because the firmware emits `> ` without a
//     trailing newline which glues it onto the next real line.
//
// Command helpers all take an optional `addr` argument. When null/undefined
// the command goes to whatever device is directly connected. When set,
// the command is prefixed with `@<addr>` so a connected Central forwards
// it to that Peripheral. See CENTRAL_PLAN.md §4.

const DEFAULT_TIMEOUT_MS = 3000;

function prefixFor(addr) {
    return (addr == null) ? '' : `@${addr} `;
}

export class Protocol {
    constructor(serial, { onLog, onSend } = {}) {
        this.serial = serial;
        this.onLog = onLog || (() => {});
        this.onSend = onSend || (() => {});
        this._pending = null;
        this._queue = Promise.resolve();
        this._unsubscribe = serial.onLine(line => this._onLine(line));
    }

    close() {
        if (this._unsubscribe) this._unsubscribe();
        if (this._pending) {
            this._pending.reject(new Error('connection closed'));
            this._pending = null;
        }
    }

    // Send a command line, collect the response, return the data lines.
    // Rejects on ERR or timeout.
    //
    // Queue sequencing detail: a rejected Promise propagates down a
    // `.then(fulfilled)` chain. Naively chaining every send to `_queue`
    // meant that once any command rejected (e.g. `@<addr> reboot` timing
    // out after the peripheral restarts), *every* subsequent queued
    // command inherited that rejection and was never actually sent —
    // surfacing as the same phantom error until the connection was
    // dropped. Fix: use `.then(onFulfilled, onRejected)` with the same
    // runner for both, and store a swallowed version of the new tail so
    // the queue is never in a rejected state for the next caller.
    send(cmd, { timeoutMs = DEFAULT_TIMEOUT_MS } = {}) {
        const runNext = () => this._doSend(cmd, timeoutMs);
        const next = this._queue.then(runNext, runNext);
        this._queue = next.catch(() => {}); // tail never in a rejected state
        return next;
    }

    _doSend(cmd, timeoutMs) {
        this.onSend(cmd);
        return new Promise((resolve, reject) => {
            const pending = { resolve, reject, lines: [] };
            pending.timer = setTimeout(() => {
                if (this._pending === pending) {
                    this._pending = null;
                    reject(new Error(`timeout waiting for response to: ${cmd}`));
                }
            }, timeoutMs);
            this._pending = pending;
            this.serial.writeLine(cmd).catch(err => {
                clearTimeout(pending.timer);
                if (this._pending === pending) this._pending = null;
                reject(err);
            });
        });
    }

    _onLine(raw) {
        let line = raw.replace(/^(> )+/, '');

        if (line.startsWith('# ')) { this.onLog(line.slice(2)); return; }
        if (line.startsWith('#'))  { this.onLog(line.slice(1).trimStart()); return; }

        if (!line) return;

        if (!this._pending) {
            this.onLog(`(unsolicited) ${line}`);
            return;
        }

        if (line === 'OK') {
            const p = this._pending;
            this._pending = null;
            clearTimeout(p.timer);
            p.resolve(p.lines);
            return;
        }
        if (line.startsWith('ERR ')) {
            const p = this._pending;
            this._pending = null;
            clearTimeout(p.timer);
            p.reject(new Error(line.slice(4)));
            return;
        }
        this._pending.lines.push(line);
    }

    // --- Convenience methods ---------------------------------------------
    //
    // All of these accept an optional `addr` — null/undefined means the
    // command goes to the directly-connected device; a number means the
    // command is prefixed with `@<addr>` so a Central forwards it.

    // Parses "WHOAMI key=val key=val ..." into an object.
    async whoami(addr = null) {
        const [line] = await this.send(`${prefixFor(addr)}whoami`);
        const m = /^WHOAMI\s+(.*)$/.exec(line || '');
        if (!m) throw new Error(`unexpected whoami response: ${line}`);
        const fields = {};
        for (const kv of m[1].split(/\s+/)) {
            const [k, v] = kv.split('=');
            if (k) fields[k] = v;
        }
        return fields;
    }

    async readConfig(addr = null) {
        const lines = await this.send(`${prefixFor(addr)}cfg`, { timeoutMs: 8000 });
        return JSON.parse(lines.join('\n'));
    }

    async writeConfig(obj, addr = null) {
        const json = JSON.stringify(obj); // compact, one line
        // 20 s is well past the normal round-trip (parse + LittleFS
        // write is <100 ms); the extra headroom matters when a legacy
        // device with a smaller USB CDC RX buffer is pacing delivery
        // byte by byte.
        await this.send(`${prefixFor(addr)}cfg ${json}`, { timeoutMs: 20000 });
    }

    async readBoard(addr = null) {
        const lines = await this.send(`${prefixFor(addr)}board`, { timeoutMs: 5000 });
        return JSON.parse(lines.join('\n'));
    }

    async writeBoard(obj, addr = null) {
        const json = JSON.stringify(obj);
        await this.send(`${prefixFor(addr)}board ${json}`, { timeoutMs: 8000 });
    }

    async reboot(addr = null) {
        if (addr == null) {
            // Directly-connected device: no OK expected; connection drops.
            this.onSend('reboot');
            await this.serial.writeLine('reboot');
        } else {
            // Through Central: the peripheral resets before emitting OK,
            // so the tunnel will time out with ERR. Treat any outcome as
            // fire-and-forget — the caller should rescan after a moment.
            try {
                await this.send(`@${addr} reboot`, { timeoutMs: 7000 });
            } catch (_) {
                // Expected: the tunneled reboot never returns a terminator.
            }
        }
    }

    // --- Central-specific helpers ----------------------------------------

    // Force an immediate I2C bus scan on a connected Central.
    async scan() {
        await this.send('scan', { timeoutMs: 5000 });
    }

    // Parse the Central's `list` output into structured entries.
    // Format example (per-line):
    //   "16 last_seen=0ms types=[2] configFetched=yes"
    //   "17 last_seen=220ms types=[1,2] configFetched=no stale"
    async listPeripherals() {
        const lines = await this.send('list', { timeoutMs: 3000 });
        const LINE_RE = /^(\d+)\s+last_seen=(\d+)ms\s+types=\[([^\]]*)\]\s+configFetched=(yes|no)(\s+stale)?/;
        const out = [];
        for (const line of lines) {
            const m = LINE_RE.exec(line);
            if (!m) continue;
            const typeIds = m[3]
                ? m[3].split(',').map(s => parseInt(s.trim(), 10)).filter(Number.isFinite)
                : [];
            out.push({
                address: parseInt(m[1], 10),
                lastSeenMs: parseInt(m[2], 10),
                typeIds,
                configFetched: m[4] === 'yes',
                stale: !!m[5],
            });
        }
        return out;
    }
}
