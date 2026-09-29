// twin-serial.js - navigator.serial on the virtual twin's copy of the web flasher.
//
// The twin's USB-Serial/JTAG is a WebSocket on the engine's own web server: ws://<host>/usj
// (protocol 1, esp32sim branch nickoscope/usj, esp-soc/src/web.rs header). This script puts a
// Serial object on navigator whose one port speaks that protocol, so ESP Web Tools 10.4.0 and the
// esptool-js 0.6.0 inside it flash the twin exactly as they flash a panel on a cable: reset into
// the ROM's download mode by DTR/RTS, the stub, the flash commands, the hard reset, Improv.
//
// Load it as a classic script, first in <head>. ESP Web Tools reads `"serial" in navigator` once,
// when its module is evaluated (esp-web-tools 10.4.0 src/install-button.ts:6), and module scripts
// run only after the document has been parsed (HTML Standard, the script element: module scripts
// are deferred), so the property is ours by then.
//
// What the port does follows the WICG Web Serial draft of 2 June 2026 (SerialPort: §4.3 getInfo,
// §4.4 open, §4.6 readable, §4.7 writable, §4.8 setSignals, §4.9 getSignals, §4.10 close) as far
// as ESP Web Tools 10.4.0, esptool-js 0.6.0 (src/webserial.ts) and improv-wifi-serial-sdk 2.8.0
// (dist/serial.js) use it. Where it differs:
//   - Nothing is dropped. Chrome errors the readable with BufferOverrunError when its buffer fills
//     (§4.6); here chunks keep queueing in the stream. While no stream exists (between a cancel and
//     the next `readable`) received bytes wait for it, the newest 1 MiB of them.
//   - The baud rate, data bits, parity and flow control are checked and then ignored: the
//     USB-Serial/JTAG takes SET_LINE_CODING and ignores it (ESP32-S3 TRM v1.8 §33.3.1, Table 33.3-1).
//   - getSignals() is always all false: the controller's interrupt endpoint never reports a modem
//     line (same TRM section). setSignals({break}) is accepted and ignored (SEND_BREAK, same table).
//   - close() with a locked stream rejects with TypeError and leaves the port open (the draft
//     leaves [[state]] at "closing"; Chromium's behaviour as far as known, not verified).
//   - Error message texts imitate Chromium's; only their names are from the draft. The one text a
//     library checks, "device has been lost", is esptool-js 0.6.0 src/webserial.ts rawRead().
//
// DTR/RTS: each member present in setSignals() sends the full pair once, DTR before RTS (§4.8
// orders them so). esptool-js 0.6.0 sets one line per call and repeats DTR after each RTS change
// (webserial.ts setRTS); the engine applies each pair in order (TRM Table 33.3-2: RTS=1 DTR=0
// resets the chip and holds it, RTS=0 DTR=1 sets the download flag). open() and close() never send
// a line change of their own, so opening the port (Improv probing) does not reset the twin. When
// the socket goes, the engine itself drops both lines, which lets a held chip run.
//
// The chooser: requestPort() shows a small dialog - the twin, a board over USB (the browser's own
// chooser, called from the click so it has its user activation), or cancel (NotFoundError, which
// ESP Web Tools answers with its "no port selected" dialog). `?twin=auto` in the page URL picks the
// twin without asking (for automated checks).
//
// For checks: window.__twinSerial = { port, serial, native, journal, counters } and
// window.__usjLog (the journal: opens, closes, every line pair sent, every engine event).
(function () {
  'use strict';
  if (window.__twinSerial) return;

  const VID = 0x303a;          // Espressif
  const PID = 0x1001;          // USB-Serial/JTAG; esptool-js picks its USB-JTAG reset only for this PID
                               // (esptool-js 0.6.0 src/esploader.ts:586-609)
  const PROTO = 1;             // the engine's /usj protocol version (hello.proto)
  const OPEN_TIMEOUT_MS = 10000;
  const CLOSE_TIMEOUT_MS = 2000;
  const SEND_HIGH_WATER = 64 * 1024;   // a write resolves once the socket's queue is below this
  const MAX_FRAME = 1 << 20;           // bytes per data frame; the engine takes up to 8 MiB a message
  const PENDING_MAX = 1 << 20;

  const native = ('serial' in navigator) ? navigator.serial : null;
  const AUTO = new URLSearchParams(location.search).get('twin') === 'auto';

  const journal = [];
  const counters = { bytesToChip: 0, bytesFromChip: 0, framesToChip: 0, framesFromChip: 0, pendingDropped: 0 };
  function note(kind, detail) {
    journal.push(Object.assign({ at: Math.round(performance.now()), kind }, detail || {}));
    if (journal.length > 5000) journal.splice(0, journal.length - 5000);
  }

  const domError = (name, message) => new DOMException(message, name);
  const lost = () => domError('NetworkError', 'The device has been lost.');
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

  // WebIDL: a dictionary argument is undefined, null or an object; [EnforceRange] integers.
  function dict(v, what) {
    if (v === undefined || v === null) return {};
    if (typeof v !== 'object' && typeof v !== 'function') throw new TypeError(`${what} is not a dictionary.`);
    return v;
  }
  function enforceRange(v, max, what) {
    const n = Number(v);
    if (!Number.isFinite(n)) throw new TypeError(`${what} is not a finite number.`);
    const t = Math.trunc(n);
    if (t < 0 || t > max) throw new TypeError(`${what} is outside the range 0..${max}.`);
    return t;
  }
  function eventHandler(proto, type) {
    const slot = Symbol(type);
    Object.defineProperty(proto, 'on' + type, {
      configurable: true, enumerable: true,
      get() { return this[slot] || null; },
      set(fn) {
        if (this[slot]) this.removeEventListener(type, this[slot]);
        this[slot] = typeof fn === 'function' ? fn : null;
        if (this[slot]) this.addEventListener(type, this[slot]);
      },
    });
  }
  function parseEvent(buf) {
    try { return JSON.parse(new TextDecoder().decode(new Uint8Array(buf, 1))); } catch (_) { return null; }
  }

  let granted = false;         // the page has chosen the twin (getPorts)
  let serial = null;           // set below; the port's disconnect also goes there

  class TwinSerialPort extends EventTarget {
    #state = 'closed';
    #bufferSize = undefined;
    #connected = true;
    #readable = null; #readController = null; #readFatal = false; #pending = []; #pendingBytes = 0;
    #writable = null; #writeController = null; #writeFatal = false;
    #pendingClose = null;
    #ws = null;
    #closedWaiter = null;
    #dtr = false; #rts = false;

    // §4.3
    getInfo() { return { usbVendorId: VID, usbProductId: PID }; }
    // §4.5
    get connected() { return this.#connected; }
    // not in the draft: for checks and the journal
    get twinState() { return this.#state; }
    get twinLines() { return { dtr: this.#dtr, rts: this.#rts }; }

    // §4.4
    async open(options) {
      const o = dict(options, 'SerialOptions');
      if (o.baudRate === undefined) throw new TypeError("Failed to read the 'baudRate' property from 'SerialOptions': Required member is undefined.");
      const baudRate = enforceRange(o.baudRate, 0xffffffff, 'baudRate');
      const bufferSize = o.bufferSize === undefined ? 255 : enforceRange(o.bufferSize, 0xffffffff, 'bufferSize');
      const dataBits = o.dataBits === undefined ? 8 : enforceRange(o.dataBits, 0xff, 'dataBits');
      const flowControl = o.flowControl === undefined ? 'none' : String(o.flowControl);
      const parity = o.parity === undefined ? 'none' : String(o.parity);
      const stopBits = o.stopBits === undefined ? 1 : enforceRange(o.stopBits, 0xff, 'stopBits');
      if (!['none', 'hardware'].includes(flowControl)) throw new TypeError(`'${flowControl}' is not a valid FlowControlType.`);
      if (!['none', 'even', 'odd'].includes(parity)) throw new TypeError(`'${parity}' is not a valid ParityType.`);
      if (this.#state !== 'closed') throw domError('InvalidStateError', 'The port is already open.');
      if (baudRate === 0) throw new TypeError('Requested baud rate must be greater than zero.');
      if (dataBits !== 7 && dataBits !== 8) throw new TypeError('Requested number of data bits must be 7 or 8.');
      if (stopBits !== 1 && stopBits !== 2) throw new TypeError('Requested number of stop bits must be 1 or 2.');
      if (bufferSize === 0) throw new TypeError('Requested buffer size must be greater than zero.');
      this.#state = 'opening';
      let hello;
      try {
        hello = await this.#connect();
      } catch (e) {
        this.#state = 'closed';
        note('open-failed', { reason: String((e && e.message) || e) });
        console.warn('[twin] open failed:', (e && e.message) || e);
        throw domError('NetworkError', 'Failed to open serial port.');
      }
      this.#state = 'opened';
      this.#bufferSize = bufferSize;
      if (!this.#connected) { this.#connected = true; this.dispatchEvent(new Event('connect')); }
      note('open', { baudRate, bufferSize, dtr: hello.dtr, rts: hello.rts, held: hello.held, reset: hello.reset });
      if (hello.reset === false) console.warn('[twin] this run ignores DTR/RTS resets (--boot app or --no-reboot): no download mode');
    }

    // The WebSocket, the claim (0x02) and the engine's hello. Resolves with the hello.
    #connect() {
      return new Promise((resolve, reject) => {
        let ws;
        try { ws = new WebSocket(`ws://${location.host}/usj`); } catch (e) { reject(e); return; }
        ws.binaryType = 'arraybuffer';
        let settled = false;
        const fail = (why) => {
          if (settled) return;
          settled = true; clearTimeout(timer);
          try { ws.close(); } catch (_) { /* already closing */ }
          reject(new Error(why));
        };
        const timer = setTimeout(() => fail('no hello from the engine'), OPEN_TIMEOUT_MS);
        ws.onopen = () => ws.send(Uint8Array.of(0x02));
        ws.onerror = () => fail('the WebSocket failed (engine not running, or a run without /usj)');
        ws.onclose = (ev) => fail(`the WebSocket closed (${ev.code})`);
        ws.onmessage = (ev) => {
          if (!(ev.data instanceof ArrayBuffer) || ev.data.byteLength < 1) return;
          if (new Uint8Array(ev.data, 0, 1)[0] !== 0x10) return;   // the hello comes before any data
          const e = parseEvent(ev.data);
          if (!e) return;
          note('event', e);
          if (e.t === 'error') { fail(`the engine said ${e.error}`); return; }
          if (e.t !== 'hello') return;
          if (e.proto !== PROTO) { fail(`the engine speaks /usj protocol ${e.proto}, this page ${PROTO}`); return; }
          settled = true; clearTimeout(timer);
          this.#dtr = !!e.dtr; this.#rts = !!e.rts;
          this.#ws = ws;
          ws.onmessage = (m) => this.#onFrame(m.data);
          ws.onerror = null;
          ws.onclose = () => { if (this.#ws === ws) this.#lost(); };
          resolve(e);
        };
      });
    }

    #onFrame(buf) {
      if (!(buf instanceof ArrayBuffer) || buf.byteLength < 1) return;
      const u8 = new Uint8Array(buf);
      if (u8[0] === 0x00) {
        if (u8.length < 2) return;
        const bytes = u8.slice(1);            // its own buffer: enqueueing transfers it
        counters.bytesFromChip += bytes.length; counters.framesFromChip++;
        this.#deliver(bytes);
      } else if (u8[0] === 0x10) {
        const e = parseEvent(buf);
        if (!e) return;
        note('event', e);
        if (e.t === 'reset' || e.t === 'release') console.info('[twin]', JSON.stringify(e));
        if (e.t === 'closed' && this.#closedWaiter) this.#closedWaiter();
      }
    }

    #deliver(bytes) {
      if (this.#readController) {
        try { this.#readController.enqueue(bytes); return; } catch (_) { /* the stream ended under us */ }
      }
      this.#pending.push(bytes); this.#pendingBytes += bytes.length;
      while (this.#pendingBytes > PENDING_MAX && this.#pending.length > 1) {
        const old = this.#pending.shift();
        this.#pendingBytes -= old.length; counters.pendingDropped += old.length;
      }
    }

    // The socket went away while the port was open: the device is lost (§4.6/§4.7 "the port was
    // disconnected"; §4.2 disconnect).
    #lost() {
      this.#ws = null;
      note('lost');
      console.warn('[twin] the engine went away: the port is lost');
      this.#readFatal = true; this.#writeFatal = true;
      if (this.#readable) { try { this.#readController.error(lost()); } catch (_) { /* ended */ } this.#readableClosed(); }
      if (this.#writable) { try { this.#writeController.error(lost()); } catch (_) { /* ended */ } this.#writableClosed(); }
      this.#pending = []; this.#pendingBytes = 0;
      this.#connected = false;
      this.dispatchEvent(new Event('disconnect', { bubbles: true }));
      if (serial) serial.dispatchEvent(new Event('disconnect'));
    }

    // "handle closing the readable stream" / "... the writable stream" (§4.6, §4.7)
    #readableClosed() {
      this.#readable = null; this.#readController = null;
      if (this.#writable === null && this.#pendingClose) this.#pendingClose();
    }
    #writableClosed() {
      this.#writable = null; this.#writeController = null;
      if (this.#readable === null && this.#pendingClose) this.#pendingClose();
    }

    // §4.6
    get readable() {
      if (this.#readable) return this.#readable;
      if (this.#state !== 'opened' || this.#readFatal) return null;
      const stream = new ReadableStream({
        type: 'bytes',
        start: (c) => { this.#readController = c; },
        // cancelAlgorithm: discard what was received and not read, then the stream is gone
        cancel: () => {
          if (this.#readable !== stream) return;
          this.#pending = []; this.#pendingBytes = 0;
          this.#readableClosed();
        },
      }, { highWaterMark: this.#bufferSize });
      this.#readable = stream;
      const waiting = this.#pending;
      this.#pending = []; this.#pendingBytes = 0;
      for (const b of waiting) this.#readController.enqueue(b);
      return stream;
    }

    // §4.7
    get writable() {
      if (this.#writable) return this.#writable;
      if (this.#state !== 'opened' || this.#writeFatal) return null;
      const stream = new WritableStream({
        start: (c) => { this.#writeController = c; },
        write: (chunk) => this.#write(chunk),
        close: async () => { await this.#drain(); if (this.#writable === stream) this.#writableClosed(); },
        abort: () => { if (this.#writable === stream) this.#writableClosed(); },
      }, { highWaterMark: this.#bufferSize, size: (c) => (c && typeof c.byteLength === 'number' ? c.byteLength : 0) });
      this.#writable = stream;
      return stream;
    }

    async #write(chunk) {
      let bytes;
      if (chunk instanceof ArrayBuffer) bytes = new Uint8Array(chunk.slice(0));
      else if (ArrayBuffer.isView(chunk)) bytes = new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength).slice();
      else throw new TypeError('The provided value is not of type (ArrayBuffer or ArrayBufferView).');
      const ws = this.#ws;
      if (!ws || ws.readyState !== WebSocket.OPEN) { this.#writeFatal = true; throw lost(); }
      for (let off = 0; off < bytes.length; off += MAX_FRAME) {
        const part = bytes.subarray(off, Math.min(bytes.length, off + MAX_FRAME));
        const frame = new Uint8Array(part.length + 1);   // frame[0] = 0x00: data
        frame.set(part, 1);
        ws.send(frame);
        counters.bytesToChip += part.length; counters.framesToChip++;
      }
      while (ws.bufferedAmount > SEND_HIGH_WATER) {
        if (ws.readyState !== WebSocket.OPEN) throw lost();
        await sleep(1);
      }
    }

    async #drain() {
      const ws = this.#ws;
      while (ws && ws.readyState === WebSocket.OPEN && ws.bufferedAmount > 0) await sleep(1);
    }

    #sendLines(ws) {
      ws.send(Uint8Array.of(0x01, (this.#dtr ? 1 : 0) | (this.#rts ? 2 : 0)));
      note('lines', { dtr: this.#dtr ? 1 : 0, rts: this.#rts ? 1 : 0 });
    }

    // §4.8
    async setSignals(signals) {
      const s = dict(signals, 'SerialOutputSignals');
      const brk = s.break === undefined ? undefined : !!s.break;
      const dtr = s.dataTerminalReady === undefined ? undefined : !!s.dataTerminalReady;
      const rts = s.requestToSend === undefined ? undefined : !!s.requestToSend;
      if (this.#state !== 'opened') throw domError('InvalidStateError', 'The port is closed.');
      if (dtr === undefined && rts === undefined && brk === undefined) throw new TypeError('Signals dictionary is empty.');
      const ws = this.#ws;
      if (!ws || ws.readyState !== WebSocket.OPEN) throw domError('NetworkError', 'Failed to set control signals.');
      if (dtr !== undefined) { this.#dtr = dtr; this.#sendLines(ws); }
      if (rts !== undefined) { this.#rts = rts; this.#sendLines(ws); }
      if (brk !== undefined) note('break', { on: brk ? 1 : 0 });
    }

    // §4.9
    async getSignals() {
      if (this.#state !== 'opened') throw domError('InvalidStateError', 'The port is closed.');
      if (!this.#ws) throw domError('NetworkError', 'Failed to get control signals.');
      return { dataCarrierDetect: false, clearToSend: false, ringIndicator: false, dataSetReady: false };
    }

    // §4.10
    async close() {
      if (this.#state !== 'opened') throw domError('InvalidStateError', 'The port is already closed.');
      if ((this.#readable && this.#readable.locked) || (this.#writable && this.#writable.locked)) {
        throw new TypeError('Cannot cancel a locked stream');
      }
      // A stream that had already ended (a write that failed) never calls back: its settled
      // promise stands for the callback.
      const r = this.#readable, w = this.#writable;
      const cancelled = r ? r.cancel().then(() => { if (this.#readable === r) this.#readableClosed(); }) : Promise.resolve();
      const aborted = w ? w.abort(domError('InvalidStateError', 'The port is closing.'))
        .then(() => { if (this.#writable === w) this.#writableClosed(); }) : Promise.resolve();
      const bothClosed = new Promise((r) => { this.#pendingClose = r; });
      if (this.#readable === null && this.#writable === null) this.#pendingClose();
      this.#state = 'closing';
      try {
        await Promise.all([cancelled, aborted, bothClosed]);
      } catch (e) {
        this.#pendingClose = null;
        this.#state = 'opened';
        throw e;
      }
      await this.#hangUp();
      this.#state = 'closed';
      this.#readFatal = false; this.#writeFatal = false;
      this.#pendingClose = null;
      this.#pending = []; this.#pendingBytes = 0;
      note('close');
    }

    // Let the port go: 0x03, answered by {"t":"closed"} once the engine has released it, so an
    // open() right after (ESP Web Tools reopens 100 ms after flashing) is never refused as busy.
    async #hangUp() {
      const ws = this.#ws;
      this.#ws = null;
      if (!ws) return;
      if (ws.readyState === WebSocket.OPEN) {
        const answered = new Promise((r) => { this.#closedWaiter = r; });
        ws.onclose = () => { if (this.#closedWaiter) this.#closedWaiter(); };
        ws.send(Uint8Array.of(0x03));
        await Promise.race([answered, sleep(CLOSE_TIMEOUT_MS)]);
        this.#closedWaiter = null;
      }
      try { ws.close(1000); } catch (_) { /* already closed */ }
    }

    // §4.11
    async forget() {
      if (this.#state === 'opened') { try { await this.close(); } catch (_) { /* locked: forgotten anyway */ } }
      this.#state = 'forgotten';
      granted = false;
    }
  }
  eventHandler(TwinSerialPort.prototype, 'connect');
  eventHandler(TwinSerialPort.prototype, 'disconnect');

  const twinPort = new TwinSerialPort();

  function matchesTwin(filters) {
    if (filters === undefined || filters.length === 0) return true;
    return filters.some((f) => f.bluetoothServiceClassId === undefined && Number(f.usbVendorId) === VID
      && (f.usbProductId === undefined || Number(f.usbProductId) === PID));
  }

  // The chooser. Resolves with the twin, with the browser's own chooser's promise, or rejects.
  function choose(options) {
    return new Promise((resolve, reject) => {
      const d = document.createElement('dialog');
      d.id = 'twin-serial-chooser';
      d.setAttribute('aria-labelledby', 'twin-serial-chooser-title');
      d.style.cssText = 'max-width:440px;width:calc(100% - 32px);padding:20px;border:1px solid #cfc7b6;border-radius:12px;'
        + 'background:#fffdf8;color:#1d1b16;font:15px/1.45 system-ui,-apple-system,sans-serif;box-shadow:0 12px 40px rgba(0,0,0,.25)';
      const btn = 'display:block;width:100%;margin:8px 0 0;padding:11px 14px;border-radius:8px;border:1px solid #cfc7b6;'
        + 'background:#fff;color:#1d1b16;font:inherit;text-align:left;cursor:pointer';
      d.innerHTML = `
        <h2 id="twin-serial-chooser-title" style="margin:0 0 6px;font-size:18px">Какой порт открыть?</h2>
        <p style="margin:0 0 6px;color:#5c574c">Это копия прошивальщика для виртуального двойника: «Двойник» — эмулятор панели на этом Mac.</p>
        <button type="button" data-choice="twin" style="${btn};background:#1d1b16;color:#fffdf8;border-color:#1d1b16">Двойник — USB-Serial/JTAG эмулятора (303A:1001)</button>
        <button type="button" data-choice="native" style="${btn}">Плата по USB…</button>
        <button type="button" data-choice="cancel" style="${btn}">Отмена</button>`;
      document.body.appendChild(d);
      let done = false;
      const finish = (then) => {
        if (done) return;
        done = true;
        try { d.close(); } catch (_) { /* not open */ }
        d.remove();
        then();
      };
      d.querySelector('[data-choice="twin"]').addEventListener('click', () => finish(() => {
        granted = true; note('choose', { port: 'twin' }); resolve(twinPort);
      }));
      const board = d.querySelector('[data-choice="native"]');
      if (native) {
        board.addEventListener('click', () => {
          // Called inside the click: the browser's chooser needs this user activation.
          let p;
          try { p = native.requestPort(options); } catch (e) { p = Promise.reject(e); }
          finish(() => { note('choose', { port: 'native' }); resolve(p); });
        });
      } else {
        board.remove();
      }
      const cancel = () => finish(() => { note('choose', { port: null }); reject(domError('NotFoundError', 'No port selected by the user.')); });
      d.querySelector('[data-choice="cancel"]').addEventListener('click', cancel);
      d.addEventListener('cancel', (e) => { e.preventDefault(); cancel(); });   // Esc
      d.showModal();
      d.querySelector('[data-choice="twin"]').focus();
    });
  }

  class TwinSerial extends EventTarget {
    // §3.1. Not an async function: the browser's chooser has to be reached synchronously from the
    // click that called this, or it loses the user activation.
    requestPort(options) {
      let o;
      try {
        o = dict(options, 'SerialPortRequestOptions');
        if (o.filters !== undefined) {
          if (o.filters === null || typeof o.filters[Symbol.iterator] !== 'function') throw new TypeError('filters is not a sequence.');
          const filters = Array.from(o.filters, (f) => dict(f, 'SerialPortFilter'));
          for (const f of filters) {
            if (f.bluetoothServiceClassId !== undefined) {
              if (f.usbVendorId !== undefined || f.usbProductId !== undefined) throw new TypeError('A filter cannot specify both bluetoothServiceClassId and usbVendorId or usbProductId.');
            } else if (f.usbVendorId === undefined) {
              throw new TypeError('A filter must provide a property to filter by.');
            }
          }
          o = Object.assign({}, o, { filters });
        }
      } catch (e) {
        return Promise.reject(e);
      }
      if (!matchesTwin(o.filters)) {
        return native ? native.requestPort(options) : Promise.reject(domError('NotFoundError', 'No port selected by the user.'));
      }
      if (AUTO) { granted = true; note('choose', { port: 'twin', auto: 1 }); return Promise.resolve(twinPort); }
      if (navigator.userActivation && !navigator.userActivation.isActive) {
        return Promise.reject(domError('SecurityError', 'Must be handling a user gesture to show a permission request.'));
      }
      return choose(options);
    }

    async getPorts() {
      const others = native ? await native.getPorts().catch(() => []) : [];
      return (granted ? [twinPort] : []).concat(others);
    }
  }
  eventHandler(TwinSerial.prototype, 'connect');
  eventHandler(TwinSerial.prototype, 'disconnect');

  serial = new TwinSerial();
  try {
    Object.defineProperty(navigator, 'serial', { value: serial, configurable: true, enumerable: true, writable: false });
  } catch (e) {
    console.error('[twin] cannot replace navigator.serial:', e);
    return;
  }
  window.__usjLog = journal;
  window.__twinSerial = { port: twinPort, serial, native, journal, counters };
  note('installed', { native: native ? 1 : 0, auto: AUTO ? 1 : 0 });
})();
