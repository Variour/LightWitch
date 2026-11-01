import { EventEmitter } from 'node:events';
import { LineSplitter, parseLine, serialize, cmd } from './protocol.js';

const HELLO_RETRY_MS = 2000;
const PING_IDLE_MS = 2000; // gateway protocol v0: ping every 2 s when nothing else goes out
const SILENCE_MS = 6000; // no line for this long: say hello again
const TICK_MS = 250;

// The console's side of the USB link: says hello on every (re)open, keeps the
// link alive with ping, matches replies to commands by `id`, and passes every
// gateway event on. Lines that are not protocol JSON are ignored.
//
// Transport: { start(), stop(), write(text) } emitting 'open' (info),
// 'close', 'data' (Buffer | string) and 'status' (German text).
export class GatewayLink extends EventEmitter {
  constructor(transport, { now = () => Date.now() } = {}) {
    super();
    this.transport = transport;
    this.now = now;
    this.state = 'closed'; // closed | handshake | connected
    this.port = null; // transport info from 'open'
    this.hello = null; // last hello reply
    this.counters = null; // last pong
    this.transportStatus = '';
    this.nextId = 1;
    this.pending = new Map();
    this.splitter = new LineSplitter();
    this.lastTx = 0;
    this.lastRx = 0;
    this.helloId = null;
    this.helloSentAt = 0;
    this.timer = null;

    transport.on('open', (info) => this._onOpen(info));
    transport.on('close', () => this._onClose());
    transport.on('data', (chunk) => this._onData(chunk));
    transport.on('status', (text) => {
      this.transportStatus = text;
      this.emit('change');
    });
  }

  start() {
    this.timer = setInterval(() => this._tick(), TICK_MS);
    this.transport.start();
  }

  async stop() {
    clearInterval(this.timer);
    this.timer = null;
    if (this.state === 'connected') {
      try {
        await this.request(cmd.bye(), { timeoutMs: 500, retries: 0 });
      } catch {
        // closing anyway
      }
    }
    this._failAll('offline');
    await this.transport.stop();
    this.state = 'closed';
  }

  get connected() {
    return this.state === 'connected';
  }

  // Sends a command and resolves with the gateway's direct reply (the event
  // carrying the same id). Rejects with { code, msg, field } on an `error`
  // reply, on timeout after `retries` resends, or 'offline'.
  request(command, { timeoutMs = 2000, retries = 1 } = {}) {
    return new Promise((resolve, reject) => {
      if (this.state !== 'connected' && !(command.ns === 'link' && command.cmd === 'hello')) {
        reject(Object.assign(new Error('Gateway nicht verbunden'), { code: 'offline' }));
        return;
      }
      const attempt = (left) => {
        const id = this.nextId++;
        const timer = setTimeout(() => {
          this.pending.delete(id);
          if (left > 0 && this.state === 'connected') attempt(left - 1);
          else reject(Object.assign(new Error(`Keine Antwort auf ${command.ns} ${command.cmd}`), { code: 'timeout' }));
        }, timeoutMs);
        this.pending.set(id, { resolve, reject, timer, command });
        this._write({ ...command, id });
      };
      attempt(retries);
    });
  }

  _write(obj) {
    const line = serialize(obj);
    this.lastTx = this.now();
    this.emit('tx', obj);
    this.transport.write(line);
  }

  _sayHello() {
    this.state = 'handshake';
    this.helloId = this.nextId++;
    this.helloSentAt = this.now();
    this._write({ ...cmd.hello(), id: this.helloId });
    this.emit('change');
  }

  _onOpen(info) {
    this.port = info;
    this.splitter = new LineSplitter();
    this.lastRx = this.now();
    this._sayHello();
  }

  _onClose() {
    this.state = 'closed';
    this.port = null;
    this._failAll('offline');
    this.emit('disconnected');
    this.emit('change');
  }

  _failAll(code) {
    for (const [id, p] of this.pending) {
      clearTimeout(p.timer);
      p.reject(Object.assign(new Error('Verbindung zum Gateway getrennt'), { code }));
      this.pending.delete(id);
    }
  }

  _onData(chunk) {
    for (const line of this.splitter.push(chunk)) {
      const ev = parseLine(line);
      if (!ev || typeof ev.ev !== 'string') continue;
      this.lastRx = this.now();
      this.emit('rx', ev);
      this._onEvent(ev);
    }
  }

  _onEvent(ev) {
    if (ev.ns === 'link' && ev.ev === 'hello') {
      if (this.state !== 'connected' || ev.id === this.helloId) {
        this.hello = ev;
        this.state = 'connected';
        this.emit('hello', ev);
        this.emit('change');
      }
      return;
    }
    if (ev.ns === 'link' && ev.ev === 'pong') {
      this.counters = {
        uptimeMs: ev.uptimeMs,
        rxFrames: ev.rxFrames,
        txFrames: ev.txFrames,
        rxOverflow: ev.rxOverflow,
        dropped: ev.dropped,
      };
      this.emit('change');
    }
    if (ev.id !== undefined && this.pending.has(ev.id)) {
      const p = this.pending.get(ev.id);
      this.pending.delete(ev.id);
      clearTimeout(p.timer);
      if (ev.ns === 'link' && ev.ev === 'error') {
        p.reject(Object.assign(new Error(ev.msg || ev.code), { code: ev.code, field: ev.field, msg: ev.msg }));
      } else {
        p.resolve(ev);
      }
    }
    this.emit('event', ev);
  }

  _tick() {
    const now = this.now();
    if (this.state === 'handshake' && now - this.helloSentAt >= HELLO_RETRY_MS) {
      this._sayHello();
      return;
    }
    if (this.state !== 'connected') return;
    if (now - this.lastRx >= SILENCE_MS) {
      // The gateway closed its side (10 s without a line) or restarted
      // without the port going away: hello again and take the replay.
      this._sayHello();
      return;
    }
    if (now - this.lastTx >= PING_IDLE_MS) {
      this.request(cmd.ping(), { timeoutMs: 1500, retries: 0 }).catch(() => {});
    }
  }
}
