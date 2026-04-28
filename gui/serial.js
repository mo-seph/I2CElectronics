// serial.js — thin WebSerial wrapper.
//
// Exposes a SerialConnection that provides line-oriented reads and writes.
// Line parser strips trailing \r, yields each line as a string (no \n).
// A single read loop drains the port; consumers subscribe via onLine()
// callbacks. Writes are queued sequentially through a single writer.

const BAUD_RATE = 115200;

export class SerialConnection {
    constructor() {
        this.port = null;
        this.reader = null;
        this.writer = null;
        this._lineCallbacks = new Set();
        this._closeCallbacks = new Set();
        this._readBuffer = '';
        this._running = false;
    }

    static supported() {
        return 'serial' in navigator;
    }

    async connect() {
        if (this.port) throw new Error('already connected');
        this.port = await navigator.serial.requestPort();
        await this.port.open({ baudRate: BAUD_RATE });
        this._startReadLoop();
    }

    async disconnect() {
        if (!this.port) return;
        this._running = false;
        try { if (this.reader) await this.reader.cancel(); } catch {}
        try { if (this.writer) this.writer.releaseLock(); } catch {}
        try { await this.port.close(); } catch {}
        this.port = null;
        this.reader = null;
        this.writer = null;
        for (const cb of this._closeCallbacks) cb();
    }

    onLine(cb)  { this._lineCallbacks.add(cb);  return () => this._lineCallbacks.delete(cb);  }
    onClose(cb) { this._closeCallbacks.add(cb); return () => this._closeCallbacks.delete(cb); }

    async writeLine(line) {
        if (!this.port) throw new Error('not connected');
        if (!this.writer) {
            this.writer = this.port.writable.getWriter();
        }
        const encoder = new TextEncoder();
        await this.writer.write(encoder.encode(line + '\n'));
    }

    async _startReadLoop() {
        this._running = true;
        const decoder = new TextDecoder();
        this.reader = this.port.readable.getReader();
        try {
            while (this._running) {
                const { value, done } = await this.reader.read();
                if (done) break;
                this._readBuffer += decoder.decode(value, { stream: true });
                let idx;
                while ((idx = this._readBuffer.indexOf('\n')) >= 0) {
                    let line = this._readBuffer.slice(0, idx);
                    this._readBuffer = this._readBuffer.slice(idx + 1);
                    if (line.endsWith('\r')) line = line.slice(0, -1);
                    for (const cb of this._lineCallbacks) cb(line);
                }
            }
        } catch (err) {
            console.warn('[serial] read loop error:', err);
        } finally {
            try { this.reader.releaseLock(); } catch {}
            this.reader = null;
        }
    }
}
