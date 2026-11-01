// In-process fake gateway for `--simulate N`: a transport for GatewayLink
// that speaks the same NDJSON as the firmware (src/game/Link.h and Bridge.h
// on the game-session branch) and runs N simulated Buzzerboards over a
// simulated radio with optional loss, latency and restarts.

import { EventEmitter } from 'node:events';
import { LineSplitter, LINE_MAX, KEY_NAMES, COLOR_NAMES, ROUND_TYPES, PHASES, CHOICE_KEYS, normalizeMac, seq8Newer } from '../protocol.js';
import { makeRandom } from './random.js';
import { SimDevice, Person } from './devices.js';

export const GW = {
  stateRepeat1: 50, stateRepeat2: 150, statePeriodOpen: 250, statePeriodIdle: 1000, stateMinGap: 50,
  inviteSessionPeriod: 1000, inviteHopPeriod: 300, inviteHopDwell: 30, invitesPerHop: 2,
  endRelease: 5000, releaseOne: 2000, admitRepeat: 250, admitTimeout: 5000, channelAnnounce: 2000,
  burstRepeat: 250, linkTimeout: 10000, maxNumber: 999,
};

const TICK_MS = 10;
const BROADCAST = 'ff:ff:ff:ff:ff:ff';
const hex2 = (n) => n.toString(16).padStart(2, '0');

// The radio: delivers frames after the latency to everyone tuned to the
// sender's channel, drops each delivery with the loss probability.
class SimRadio {
  constructor(world, { loss = 0, latencyMs = 0 }) {
    this.world = world;
    this.loss = loss;
    this.latencyMs = latencyMs;
    this.queue = [];
  }

  _delay() {
    return this.latencyMs > 0 ? this.world.rnd.int(1, this.latencyMs) : 0;
  }

  fromBridge(frame, channel) {
    for (const d of this.world.devices) {
      if (this.world.rnd.chance(this.loss)) continue;
      this.queue.push({ at: this.world.now() + this._delay(), channel, to: d, frame });
    }
  }

  fromDevice(dev, frame) {
    if (this.world.rnd.chance(this.loss)) return;
    this.queue.push({ at: this.world.now() + this._delay(), channel: dev.channel, to: 'bridge', frame, rssi: dev.rssi });
  }

  tick(now) {
    if (!this.queue.length) return;
    const due = [];
    const rest = [];
    for (const q of this.queue) (q.at <= now ? due : rest).push(q);
    this.queue = rest;
    due.sort((a, b) => a.at - b.at);
    for (const q of due) {
      if (q.to === 'bridge') {
        if (this.world.gateway.radioChannel === q.channel) this.world.gateway.onFrame(q.frame, q.rssi);
      } else if (q.to.channel === q.channel) {
        q.to.onFrame(q.frame, now);
      }
    }
  }
}

export class FakeGateway extends EventEmitter {
  constructor({ devices = 50, seed = 1, loss = 0, latencyMs = 0, reboots = false, timing = {}, fw = '1a2b3c4d' } = {}) {
    super();
    this.opts = { devices, seed, loss, latencyMs, reboots };
    this.t = { ...GW, ...timing };
    this.fw = fw;
    this.mac = '02:00:00:00:00:01';
    this.rnd = makeRandom(seed);
    this.t0 = Date.now(); // world clock of the simulation
    this.bootAt = 0; // gateway start on that clock
    this.timer = null;
    this.input = [];
    this.out = [];
    this.flushQueued = false;

    // link
    this.splitter = new LineSplitter(LINE_MAX + 1);
    this.active = false;
    this.closing = false;
    this.everBridge = false;
    this.lastRx = 0;
    this.dropped = 0;
    this.ownNumber = 0;

    // bridge
    this.bActive = false;
    this.ending = false;
    this.session = 0;
    this.channel = 0;
    this.fleet = 0;
    this.login = false;
    this.pendingChannel = 0;
    this.radioChannel = 0;
    this.offChannel = false;
    this.round = this._emptyRound();
    this.devs = new Map(); // number -> entry
    this.maxNumber = 0;
    this.arrivals = 0;
    this.admits = [];
    this.releases = [];
    this.rxFrames = 0;
    this.txFrames = 0;
    this.nextStateAt = 0;
    this.lastStateAt = -Infinity;
    this.burstStep = 0;

    this.world = {
      rnd: this.rnd,
      fw,
      now: () => this.now(),
      gateway: this,
      devices: [],
    };
    this.world.radio = new SimRadio(this.world, { loss, latencyMs });
    this.world.person = new Person(this.world);
    this._makeDevices(devices);
  }

  now() {
    return Date.now() - this.t0;
  }

  get devices() {
    return this.world.devices;
  }

  _makeDevices(n) {
    const rnd = this.rnd;
    for (let i = 1; i <= n; i++) {
      let storedN = i;
      if (rnd.chance(0.08)) storedN = 0; // never numbered
      this.world.devices.push(new SimDevice(this.world, {
        index: i,
        mac: `02:b0:00:00:${hex2(i >> 8)}:${hex2(i & 0xff)}`,
        storedN,
        channel: rnd.int(1, 13),
        battery: rnd.int(35, 100),
        rssi: rnd.int(-82, -45),
      }));
    }
    // One sticker mix-up: two devices with the same stored number.
    if (n >= 10) this.world.devices[n - 1].number = 3;
  }

  // ── Transport interface ──────────────────────────────────────────────────
  start() {
    this.timer = setInterval(() => this._tick(), TICK_MS);
    this.emit('status', `Simulator mit ${this.opts.devices} Geräten`);
    setTimeout(() => this.emit('open', { path: 'simulator', sim: true }), 50);
    if (this.opts.reboots) this._scheduleReboot();
  }

  async stop() {
    clearInterval(this.timer);
    clearTimeout(this.rebootTimer);
    this.timer = null;
  }

  // Like the firmware's loop(): input waits for the next pass, which reads
  // every line that arrived, then runs the bridge once.
  write(text) {
    this.input.push(text);
  }

  // Restart the gateway: the port goes away and comes back, all state is lost.
  restart() {
    this.emit('close');
    Object.assign(this, {
      active: false, closing: false, everBridge: false, bActive: false, ending: false, session: 0,
      round: this._emptyRound(), devs: new Map(), maxNumber: 0, admits: [], releases: [], login: false,
      pendingChannel: 0, offChannel: false, radioChannel: 0, bootAt: this.now(),
      lastStateAt: -Infinity, nextStateAt: 0, burstStep: 0, rxFrames: 0, txFrames: 0,
      input: [], out: [], splitter: new LineSplitter(LINE_MAX + 1),
    });
    setTimeout(() => this.emit('open', { path: 'simulator', sim: true }), 300);
  }

  _scheduleReboot() {
    this.rebootTimer = setTimeout(() => {
      const members = this.devices.filter((d) => d.state === 'member');
      if (members.length) this.rnd.pick(members).reboot(this.now());
      this._scheduleReboot();
    }, this.rnd.int(30000, 60000));
  }

  _line(obj) {
    if (!this.active) return;
    this.out.push(JSON.stringify(obj) + '\n');
    if (!this.flushQueued) {
      this.flushQueued = true;
      setImmediate(() => {
        this.flushQueued = false;
        const text = this.out.join('');
        this.out = [];
        if (text) this.emit('data', text);
      });
    }
  }

  // ── Link: commands ───────────────────────────────────────────────────────
  _processLine(raw) {
    const now = this.now();
    if (this.active) this.lastRx = now;
    if (raw.length > LINE_MAX) {
      this._error(undefined, 'line_too_long', null, null, null, 'line longer than 512 bytes');
      return;
    }
    const line = raw.endsWith('\r') ? raw.slice(0, -1) : raw;
    if (!line) return;
    let o;
    try {
      o = JSON.parse(line);
    } catch {
      o = null;
    }
    if (!o || typeof o !== 'object' || Array.isArray(o)) {
      this._error(undefined, 'bad_json', null, null, null, 'not a JSON object');
      return;
    }
    const ns = typeof o.ns === 'string' ? o.ns : null;
    const c = typeof o.cmd === 'string' ? o.cmd : null;
    const isHello = ns === 'link' && c === 'hello';
    if (!this.active && !isHello) return; // a device becomes the bridge with `link hello`
    if (!this.active) {
      this.active = true;
      this.closing = true;
      this.lastRx = now;
    }
    let id;
    if (o.id !== undefined) {
      if (typeof o.id === 'string' && o.id.length <= 16) id = o.id;
      else if (Number.isInteger(o.id)) id = o.id;
      else return this._error(undefined, 'bad_field', ns, c, 'id', 'id must be an integer or a string of 1-16 chars');
    }
    if (!ns) return this._error(id, 'bad_field', null, null, 'ns', 'ns must be a string');
    if (ns !== 'link' && ns !== 'game') return this._error(id, 'unknown_ns', ns, c, null, 'supported namespaces: link, game');
    if (!c) return this._error(id, 'bad_field', ns, null, 'cmd', 'cmd must be a string');
    if (ns === 'link') return this._linkCmd(o, c, id, now);
    return this._gameCmd(o, c, id, now);
  }

  _ev(ns, ev, id, fields = {}) {
    const o = { ns, ev };
    if (id !== undefined) o.id = id;
    this._line(Object.assign(o, fields));
  }

  _error(id, code, refNs, refCmd, field, msg) {
    const f = { code };
    if (refNs) f.refNs = refNs;
    if (refCmd) f.refCmd = refCmd;
    if (field) f.field = field;
    if (msg) f.msg = msg;
    this._ev('link', 'error', id, f);
  }

  _linkCmd(o, c, id, now) {
    if (c === 'hello') {
      if (!Number.isInteger(o.link)) return this._error(id, 'bad_field', 'link', 'hello', 'link', 'link must be an integer');
      if (o.link !== 0) return this._error(id, 'version', 'link', 'hello', null, 'supported link version: 0');
      this.everBridge = true;
      this.active = true;
      this.closing = false;
      this.lastRx = now;
      this._ev('link', 'hello', id, { link: 0, spaces: { link: 0, game: 0 }, fw: this.fw, mac: this.mac, proto: 1, uptimeMs: now - this.bootAt });
      this._sendSession(undefined, false);
      if (this.bActive) this._sendState(undefined, false, now);
      this._markAllDirty();
      return undefined;
    }
    if (c === 'ping') {
      return this._ev('link', 'pong', id, { uptimeMs: now - this.bootAt, rxFrames: this.rxFrames, txFrames: this.txFrames, rxOverflow: 0, dropped: this.dropped });
    }
    if (c === 'bye') {
      this._ev('link', 'bye', id);
      setImmediate(() => {
        this.active = false;
      });
      return undefined;
    }
    return this._error(id, 'unknown_cmd', 'link', c, null, 'link commands: hello, ping, bye');
  }

  _gameCmd(o, c, id, now) {
    if (c === 'session') return this._cmdSession(o, id, now);
    if (c === 'end') {
      if (!this.bActive || this.ending) return this._error(id, 'no_session', 'game', c, null, 'no active session');
      this.ending = true;
      this.endId = id;
      this.endUntil = now + this.t.endRelease;
      this.nextEndReleaseAt = now;
      this.login = false;
      this.pendingChannel = 0;
      return undefined;
    }
    if (c === 'admit') return this._cmdAdmit(o, id, now);
    if (c === 'release') return this._cmdRelease(o, id, now);
    if (c === 'round') return this._cmdRound(o, id, now);
    if (c === 'number') {
      if ('n' in o && o.n !== null && !(Number.isInteger(o.n) && o.n >= 1 && o.n <= 999)) {
        return this._error(id, 'bad_field', 'game', 'number', 'n', 'n must be 1-999 or null');
      }
      if ('n' in o) this.ownNumber = o.n === null ? 0 : o.n;
      return this._ev('game', 'number', id, { n: this.ownNumber || null });
    }
    return this._error(id, 'unknown_cmd', 'game', c, null, 'game commands: session, end, admit, release, round, number');
  }

  _cmdSession(o, id, now) {
    const bad = (f, m) => this._error(id, 'bad_field', 'game', 'session', f, m);
    if (!Number.isInteger(o.session) || o.session < 1 || o.session > 65535) return bad('session', 'session must be 1-65535');
    if (!Number.isInteger(o.channel) || o.channel < 1 || o.channel > 13) return bad('channel', 'channel must be 1-13');
    if ('fleet' in o && !(Number.isInteger(o.fleet) && o.fleet >= 0 && o.fleet <= 65535)) return bad('fleet', 'fleet must be 0-65535');
    if (typeof o.login !== 'boolean') return bad('login', 'login must be a boolean');
    const fleet = o.fleet ?? 0;
    if (this.ending) return this._error(id, 'no_session', 'game', 'session', null, 'session is ending');
    if (this.bActive && o.session !== this.session) return this._error(id, 'session_active', 'game', 'session', null, 'another session is running; end it first');
    if (!this.bActive) {
      this.bActive = true;
      this.session = o.session;
      this.channel = o.channel;
      this.fleet = fleet;
      this.login = o.login;
      this.pendingChannel = 0;
      this.admits = [];
      this.releases = [];
      this.devs = new Map();
      this.maxNumber = 0;
      this.arrivals = 0;
      this.offChannel = false;
      this.radioChannel = o.channel;
      this.round = this._emptyRound();
      this._markStateChanged(now);
      this.nextInviteAt = now;
      this.nextHopAt = now + this.t.inviteHopPeriod;
      this.hopIdx = o.channel;
    } else {
      this.fleet = fleet;
      if (o.login && !this.login) {
        this.nextInviteAt = now;
        this.nextHopAt = now + this.t.inviteHopPeriod;
      }
      this.login = o.login;
      if (o.channel !== this.channel && o.channel !== this.pendingChannel) {
        this.pendingChannel = o.channel;
        this.announceUntil = now + this.t.channelAnnounce;
        this.nextAnnounceAt = now;
      } else if (o.channel === this.channel) {
        this.pendingChannel = 0;
      }
    }
    return this._sendSession(id, false);
  }

  _cmdAdmit(o, id, now) {
    if (!this.bActive || this.ending) return this._error(id, 'no_session', 'game', 'admit', null, 'no active session');
    const mac = normalizeMac(o.mac);
    if (!mac) return this._error(id, 'bad_field', 'game', 'admit', 'mac', 'mac must look like 02:00:00:00:00:11');
    if (!Number.isInteger(o.n) || o.n < 1 || o.n > 999) return this._error(id, 'bad_field', 'game', 'admit', 'n', 'n must be 1-999');
    let slot = this.admits.find((a) => a.mac === mac);
    if (!slot) {
      slot = { mac };
      this.admits.push(slot);
    }
    Object.assign(slot, { n: o.n, id, startedAt: now, nextAt: now });
    this._serviceAdmits(now);
    return undefined;
  }

  _cmdRelease(o, id, now) {
    if (!this.bActive || this.ending) return this._error(id, 'no_session', 'game', 'release', null, 'no active session');
    const mac = normalizeMac(o.mac);
    if (!mac) return this._error(id, 'bad_field', 'game', 'release', 'mac', 'mac must look like 02:00:00:00:00:11');
    let slot = this.releases.find((r) => r.mac === mac);
    if (!slot) {
      slot = { mac };
      this.releases.push(slot);
    }
    slot.until = now + this.t.releaseOne;
    slot.nextAt = now;
    this.admits = this.admits.filter((a) => a.mac !== mac);
    for (const [n, d] of this.devs) if (d.mac === mac) this._clearDevice(n);
    this._serviceReleases(now);
    return this._ev('game', 'released', id, { mac });
  }

  _parseRound(o, id) {
    const bad = (field, msg) => {
      this._error(id, 'bad_field', 'game', 'round', field, msg);
      return null;
    };
    const c = { winner: 0, correct: 0, min: 0, max: 0, redMax: 0, greenMin: 0, keys: {}, openInMs: undefined };
    if (!Number.isInteger(o.round) || o.round < 1 || o.round > 65535) return bad('round', 'round must be 1-65535');
    c.round = o.round;
    if (!ROUND_TYPES.includes(o.type)) return bad('type', 'type must be idle, race, scale or choice');
    c.type = o.type;
    const idle = c.type === 'idle';
    if (idle) {
      if ('phase' in o) return bad('phase', 'omit phase for idle');
      c.phase = 'closed';
      if ('keys' in o) return bad('keys', 'omit keys for idle');
    } else {
      if (!PHASES.includes(o.phase)) return bad('phase', 'phase must be announced, open or closed');
      c.phase = o.phase;
      if (!o.keys || typeof o.keys !== 'object' || Array.isArray(o.keys)) return bad('keys', 'keys must be an object');
      for (const [k, col] of Object.entries(o.keys)) {
        if (['l1', 'l2', 'r1', 'r2'].includes(k)) return bad('keys', 'l1, l2, r1, r2 are reserved in v0');
        if (!KEY_NAMES.includes(k)) return bad('keys', 'keys: buzz, up, down, left, right, enter');
        if (!COLOR_NAMES.includes(col)) return bad('keys', 'colors: white, red, green, yellow, blue, magenta, cyan, orange');
        c.keys[k] = col;
      }
      const names = Object.keys(c.keys).sort().join(',');
      let ok;
      let rule;
      if (c.type === 'race') {
        ok = names === 'buzz';
        rule = 'race takes exactly buzz';
      } else if (c.type === 'scale') {
        ok = names === 'down,up';
        rule = 'scale takes exactly up and down';
      } else {
        ok = [2, 3, 4].some((k) => CHOICE_KEYS.slice(0, k).sort().join(',') === names);
        rule = 'choice takes up, right, down, left in this order, 2-4 keys';
      }
      if (!ok) return bad('keys', rule);
    }
    const scaleKeys = ['min', 'max', 'redMax', 'greenMin'];
    if (c.type !== 'scale') {
      for (const k of scaleKeys) if (k in o) return bad(k, 'only for scale');
    } else {
      const int = (k, lo, hi) => Number.isInteger(o[k]) && o[k] >= lo && o[k] <= hi;
      if (!int('min', 0, 98)) return bad('min', 'min must be 0-98');
      if (!int('max', o.min + 1, 99)) return bad('max', 'max must be > min and <= 99');
      if (o.max - o.min + 1 > 24) return bad('max', 'max - min + 1 must be <= 24');
      if (!int('redMax', o.min, o.max)) return bad('redMax', 'redMax must be >= min');
      if (!int('greenMin', o.redMax + 1, o.max)) return bad('greenMin', 'greenMin must be > redMax and <= max');
      Object.assign(c, { min: o.min, max: o.max, redMax: o.redMax, greenMin: o.greenMin });
    }
    if ('openInMs' in o) {
      if (!Number.isInteger(o.openInMs) || o.openInMs < 0 || o.openInMs > 5000) return bad('openInMs', 'openInMs must be 0-5000');
      if (idle || c.phase !== 'open') return bad('openInMs', 'only with phase open');
      c.openInMs = o.openInMs;
    }
    if ('winner' in o) {
      if (c.type !== 'race') return bad('winner', 'only for race');
      if (!Number.isInteger(o.winner) || o.winner < 0 || o.winner > 999) return bad('winner', 'winner must be 0-999');
      c.winner = o.winner;
    }
    if ('correct' in o) {
      if (c.type !== 'choice' || c.phase !== 'closed') return bad('correct', 'only for choice in phase closed');
      const answers = Object.keys(c.keys).length;
      if (!Number.isInteger(o.correct) || o.correct < 1 || o.correct > answers) return bad('correct', 'correct must be 1 to the number of answers');
      c.correct = o.correct;
    }
    return c;
  }

  _cmdRound(o, id, now) {
    if (!this.bActive || this.ending) return this._error(id, 'no_session', 'game', 'round', null, 'no active session');
    const c = this._parseRound(o, id);
    if (!c) return undefined;
    // New round: the old round's values go out first.
    if (c.round !== this.round.id) this._flushTable();
    const r = this.round;
    let changed = false;
    const keysEqual = JSON.stringify(Object.entries(c.keys).sort()) === JSON.stringify(Object.entries(r.keys).sort());
    if (c.round !== r.id) {
      this.round = {
        ...this._emptyRound(), id: c.round, seq: 1, type: c.type, phase: c.phase, keys: c.keys,
        min: c.min, max: c.max, redMax: c.redMax, greenMin: c.greenMin, winner: c.winner, correct: c.correct,
      };
      if (c.type !== 'idle') this._enterPhase(c.phase, c, now);
      for (const n of this.devs.keys()) this._clearValue(n);
      this.arrivals = 0;
      changed = true;
      this._markStateChanged(now);
    } else {
      if (c.type !== r.type || !keysEqual || c.min !== r.min || c.max !== r.max || c.redMax !== r.redMax || c.greenMin !== r.greenMin) {
        return this._error(id, 'round_fixed', 'game', 'round', null, 'type, keys and scale of a running round are fixed');
      }
      const phase = r.type === 'idle' ? 'closed' : c.phase;
      if (PHASES.indexOf(phase) < PHASES.indexOf(r.phase)) return this._error(id, 'bad_phase', 'game', 'round', null, 'phases only move forward');
      if (!(phase === r.phase && c.winner === r.winner && c.correct === r.correct)) {
        if (phase !== r.phase) this._enterPhase(phase, c, now);
        r.phase = phase;
        r.winner = c.winner;
        r.correct = c.correct;
        r.seq++;
        changed = true;
        this._markStateChanged(now);
      }
    }
    return this._sendState(id, changed, now);
  }

  _sendSession(id, forceInactive) {
    const active = this.bActive && !forceInactive;
    const show = active || forceInactive;
    this._ev('game', 'session', id, {
      session: show ? this.session : 0,
      channel: show ? (this.pendingChannel || this.channel) : 0,
      fleet: show ? this.fleet : 0,
      login: active && this.login,
      active,
    });
  }

  _sendState(id, changed, now) {
    const r = this.round;
    this._ev('game', 'state', id, {
      round: r.id, seq: r.seq, type: r.type, phase: r.type === 'idle' ? null : r.phase, changed,
      sinceOpenMs: r.opened ? now - r.openAt : null,
      closedAtMs: r.closedValid ? r.closedAtMs : null,
    });
  }

  // ── Bridge ───────────────────────────────────────────────────────────────
  _emptyRound() {
    return {
      id: 0, seq: 1, type: 'idle', phase: 'closed', keys: {}, min: 0, max: 0, redMax: 0, greenMin: 0,
      winner: 0, correct: 0, opened: false, openAt: 0, closedValid: false, closedAtMs: 0,
    };
  }

  _enterPhase(phase, c, now) {
    const r = this.round;
    if (phase === 'open' && !r.opened) {
      r.opened = true;
      r.openAt = now + (c.openInMs ?? 0);
    }
    if (phase === 'closed' && r.opened && !r.closedValid) {
      r.closedValid = true;
      r.closedAtMs = now - r.openAt;
    }
  }

  _markStateChanged(now) {
    this.burstStep = 0;
    this.nextStateAt = Math.max(now, this.lastStateAt + this.t.stateMinGap);
  }

  _dev(n) {
    let d = this.devs.get(n);
    if (!d) {
      d = { mac: null, hasValue: false, vseqReset: false, late: false, value: null, vseq: 0, t: 0, arr: 0, hasStatus: false, dirtyValue: false, dirtyDevice: false };
      this.devs.set(n, d);
    }
    if (n > this.maxNumber) this.maxNumber = n;
    return d;
  }

  _clearValue(n) {
    const d = this.devs.get(n);
    if (!d) return;
    Object.assign(d, { hasValue: false, vseqReset: false, late: false, value: null, vseq: 0, arr: 0, t: 0, dirtyValue: false });
  }

  _clearDevice(n) {
    this.devs.delete(n);
  }

  _markAllDirty() {
    for (const d of this.devs.values()) {
      if (d.mac && d.hasStatus) d.dirtyDevice = true;
      if (d.hasValue) d.dirtyValue = true;
    }
  }

  onFrame(f, rssi) {
    this.rxFrames++;
    if (!this.bActive || this.ending || f.origin === 0 || f.session !== this.session) return;
    if (f.type === 'report') this._onReport(f, rssi);
    else if (f.type === 'status') this._onStatus(f, rssi);
  }

  _onReport(f, rssi) {
    const n = f.origin;
    if (n < 1 || n > 999) return;
    if (f.roundId !== this.round.id || this.round.type === 'idle') return;
    const d = this._dev(n);
    if (d.hasValue && !d.vseqReset && !seq8Newer(f.valueSeq, d.vseq)) return;
    if (!d.hasValue) d.arr = ++this.arrivals;
    Object.assign(d, {
      hasValue: true, vseqReset: false, value: f.value, vseq: f.valueSeq, t: f.pressOffsetMs,
      late: this.round.closedValid && f.pressOffsetMs > this.round.closedAtMs,
      valueRssi: rssi, valueBattery: f.battery, dirtyValue: true,
    });
  }

  _onStatus(f, rssi) {
    const n = f.origin;
    const hasNumber = n >= 1 && n <= 999;
    if (f.join) {
      const existing = this.devs.get(n);
      if (hasNumber && f.rejoin && existing && existing.mac === f.mac) existing.vseqReset = true;
      this._ev('game', 'join', undefined, {
        mac: f.mac, n: hasNumber ? n : null, fw: f.fw, proto: 1, fleet: f.fleet,
        bat: f.battery, chg: f.charging, rssi, pressed: f.pressed, rejoin: f.rejoin,
      });
      return;
    }
    if (!hasNumber) return;
    for (const [k, other] of this.devs) if (k !== n && other.mac === f.mac) this._clearDevice(k);
    const prev = this.devs.get(n);
    if (prev && prev.mac && prev.mac !== f.mac) this._clearDevice(n);
    const d = this._dev(n);
    Object.assign(d, {
      mac: f.mac, hasStatus: true, fw: f.fw, battery: f.battery, charging: f.charging, rssi,
      lastRound: f.lastRoundId, lastSeq: f.lastStateSeq, dirtyDevice: true,
    });
    const i = this.admits.findIndex((a) => a.mac === f.mac && a.n === n);
    if (i >= 0) {
      const done = this.admits[i];
      this.admits.splice(i, 1);
      this._ev('game', 'admitted', done.id, { mac: done.mac, n: done.n });
    }
  }

  _send(frame) {
    this.txFrames++;
    this.world.radio.fromBridge({ ...frame, origin: 0, fleet: this.fleet, session: this.session }, this.radioChannel);
  }

  _sendStateFrame(now) {
    const r = this.round;
    const echo = [];
    for (let n = 1; n <= this.maxNumber; n++) {
      const d = this.devs.get(n);
      echo.push(d && d.hasValue ? d.value : 0xff);
    }
    this._send({
      type: 'state', roundId: r.id, stateSeq: r.seq, roundType: r.type, phase: r.phase, keys: r.keys,
      min: r.min, max: r.max, redMax: r.redMax, greenMin: r.greenMin, winner: r.winner, correct: r.correct,
      openOffsetMs: r.opened ? now - r.openAt : 0, echoFirst: 1, echo,
    });
    this.lastStateAt = now;
    const period = r.type !== 'idle' && r.phase === 'open' ? this.t.statePeriodOpen : this.t.statePeriodIdle;
    if (this.burstStep === 0) {
      this.burstAnchor = now;
      this.nextStateAt = now + this.t.stateRepeat1;
      this.burstStep = 1;
    } else if (this.burstStep === 1) {
      this.nextStateAt = this.burstAnchor + this.t.stateRepeat2;
      this.burstStep = 2;
    } else {
      this.nextStateAt = now + period;
      this.burstStep = 3;
    }
  }

  _serviceAdmits(now) {
    for (const a of [...this.admits]) {
      if (now - a.startedAt >= this.t.admitTimeout) {
        this.admits.splice(this.admits.indexOf(a), 1);
        this._error(a.id, 'admit_timeout', 'game', 'admit', null, 'device did not confirm the admission within 5 s');
        continue;
      }
      if (this.offChannel || now < a.nextAt) continue;
      this._send({ type: 'admit', mac: a.mac, number: a.n });
      a.nextAt = now + this.t.admitRepeat;
    }
  }

  _serviceReleases(now) {
    for (const r of [...this.releases]) {
      if (now >= r.until) {
        this.releases.splice(this.releases.indexOf(r), 1);
        continue;
      }
      if (this.offChannel || now < r.nextAt) continue;
      this._send({ type: 'release', mac: r.mac });
      r.nextAt = now + this.t.burstRepeat;
    }
  }

  _bridgeTick(now) {
    if (!this.bActive) return;
    if (this.offChannel) {
      if (now < this.hopReturnAt) return;
      this.offChannel = false;
      this.radioChannel = this.channel;
    }
    if (this.ending) {
      if (now >= this.endUntil) {
        this.bActive = false;
        this.ending = false;
        this.devs = new Map();
        this.maxNumber = 0;
        this.round = this._emptyRound();
        this._sendSession(this.endId, true);
        return;
      }
      if (now >= this.nextEndReleaseAt) {
        this._send({ type: 'release', mac: BROADCAST });
        this.nextEndReleaseAt = now + this.t.burstRepeat;
      }
      return;
    }
    if (this.pendingChannel) {
      if (now >= this.announceUntil) {
        this.channel = this.pendingChannel;
        this.pendingChannel = 0;
        this.radioChannel = this.channel;
        this._markStateChanged(now);
      } else if (now >= this.nextAnnounceAt) {
        this._send({ type: 'channel', channel: this.pendingChannel, switchInMs: this.announceUntil - now });
        this.nextAnnounceAt = now + this.t.burstRepeat;
      }
    }
    if (now >= this.nextStateAt) this._sendStateFrame(now);
    this._serviceAdmits(now);
    this._serviceReleases(now);
    if (this.login && !this.pendingChannel) {
      if (now >= this.nextInviteAt) {
        this._send({ type: 'invite', channel: this.channel });
        this.nextInviteAt = now + this.t.inviteSessionPeriod;
      }
      if (now >= this.nextHopAt) {
        this.hopIdx = (this.hopIdx % 13) + 1;
        if (this.hopIdx === this.channel) this.hopIdx = (this.hopIdx % 13) + 1;
        this.radioChannel = this.hopIdx;
        for (let i = 0; i < this.t.invitesPerHop; i++) this._send({ type: 'invite', channel: this.channel });
        this.offChannel = true;
        this.hopReturnAt = now + this.t.inviteHopDwell;
        this.nextHopAt = now + this.t.inviteHopPeriod;
      }
    }
  }

  // Writes every changed participant and value as one line each (the
  // firmware writes them straight from its table, never from a queue).
  _flushTable() {
    if (!this.active || this.closing) return;
    for (const [n, d] of [...this.devs.entries()].sort((a, b) => a[0] - b[0])) {
      if (d.dirtyDevice) {
        d.dirtyDevice = false;
        this._ev('game', 'device', undefined, {
          n, mac: d.mac, fw: d.fw, proto: 1, bat: d.battery, chg: !!d.charging, rssi: d.rssi, round: d.lastRound, seq: d.lastSeq,
        });
      }
      if (d.dirtyValue) {
        d.dirtyValue = false;
        this._ev('game', 'value', undefined, {
          n, round: this.round.id, v: d.value, vseq: d.vseq, t: d.t, arr: d.arr, late: d.late, rssi: d.valueRssi, bat: d.valueBattery,
        });
      }
    }
  }

  _tick() {
    const input = this.input;
    this.input = [];
    for (const text of input) for (const line of this.splitter.push(text)) this._processLine(line);
    const now = this.now();
    if (this.active && !this.closing && now - this.lastRx >= this.t.linkTimeout) this.active = false;
    this.world.radio.tick(now);
    for (const d of this.devices) d.tick(now);
    this._bridgeTick(now);
    this._flushTable();
    if (this.closing) this.active = false;
  }
}
