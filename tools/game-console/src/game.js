import { EventEmitter } from 'node:events';
import { cmd, keysFor, normalizeMac } from './protocol.js';

// Fixed values in v0 (round format, "Feste Werte in v0", console part).
export const TIMING = {
  afterCloseMs: 1000, // Nachlauf between closing and the next round
  raceOpenInMs: 1000, // race: keys free 1 s after the open command
  raceWinnerMs: 1000, // race: winner 1 s after the first press arrived
  tieMs: 5, // race: presses less than 5 ms apart share a rank
  offlineMs: 40000, // device offline after 40 s without device or value
};

const MAX_NUMBER = 999;
const MAX_MESSAGES = 30;
const PRESS_HIGHLIGHT_MS = 8000;
const RELEASE_GRACE_MS = 5000;

export class ActionError extends Error {}

// The console: single source of truth for session, participants, round plan,
// the running round and every value. Decides on admits, assigns numbers and
// pushes the session and round state to the gateway over the link. The
// gateway only repeats what the console says; after every hello the console
// compares the gateway's replay with its own state and sends what differs.
export class GameConsole extends EventEmitter {
  constructor({ link, channel = 6, fleet = 0, autoAdmit = true, mode = 'serial', now = () => Date.now(), random = Math.random }) {
    super();
    this.link = link;
    this.mode = mode;
    this.now = now;
    this.random = random;
    this.settings = { channel, fleet, autoAdmit };
    this.plan = { title: '', rounds: [], next: 0, insertAt: 0 };
    this.session = null;
    this.devices = new Map(); // mac -> device
    this.round = null; // running round (one start of a definition)
    this.prevRound = null; // the one before, for values that arrive late
    this.lastCloseAt = 0;
    this.lastPressed = null;
    this.messages = [];
    this.warning = null;
    this.ended = false;
    this.roundIdNext = 1 + Math.floor(random() * 65535);
    this.queue = Promise.resolve();
    this.winnerTimer = null;
    this.released = new Map(); // mac -> time of the release

    link.on('hello', (ev) => this._onHello(ev));
    link.on('event', (ev) => this._onEvent(ev));
    link.on('change', () => this._changed());
  }

  // ── Input from the skill ─────────────────────────────────────────────────
  // `input` is a validated { kind, value } from validateInput().
  load({ kind, value }) {
    if (kind === 'session') {
      this.plan = { title: value.title, rounds: value.rounds.map((r) => ({ ...r })), next: 0, insertAt: 0 };
      const msg = `Sitzung "${value.title || 'ohne Titel'}" mit ${value.rounds.length} Runde(n) geladen.`;
      this._msg('info', msg);
      return { ok: true, message: msg };
    }
    const i = this.plan.rounds.findIndex((r) => r.id === value.id);
    if (i >= 0) {
      this.plan.rounds[i] = { ...value };
      const running = this.round && this.round.def && this.round.def.id === value.id && this.round.phase !== 'closed';
      const msg = `Runde "${value.id}" ersetzt${running ? ' (der laufende Durchgang bleibt unverändert)' : ''}.`;
      this._msg('info', msg);
      return { ok: true, message: msg };
    }
    const at = Math.min(Math.max(this.plan.insertAt, this.plan.next), this.plan.rounds.length);
    this.plan.rounds.splice(at, 0, { ...value });
    this.plan.insertAt = at + 1;
    const msg = `Runde "${value.id}" als nächste eingereiht (Position ${at + 1} von ${this.plan.rounds.length}).`;
    this._msg('info', msg);
    return { ok: true, message: msg };
  }

  // ── Presenter actions ────────────────────────────────────────────────────
  // Runs one action after the previous one finished. Resolves with
  // { ok: true, message } or { ok: false, error } (German text).
  act(action, args = {}) {
    const run = async () => {
      const fn = this.actions[action];
      if (!fn) throw new ActionError(`Unbekannte Aktion "${action}".`);
      const message = await fn.call(this, args);
      return { ok: true, message: message ?? 'OK' };
    };
    const p = this.queue.then(run).catch((err) => {
      if (!(err instanceof ActionError)) this._msg('error', `Interner Fehler: ${err.message}`);
      return { ok: false, error: err.message };
    });
    this.queue = p.then(() => {});
    return p;
  }

  get actions() {
    return {
      next: this._next,
      'session-start': this._startSession,
      'login-open': () => this._setLogin(true),
      'login-close': () => this._setLogin(false),
      announce: ({ id } = {}) => this._announce(id),
      open: this._open,
      close: this._close,
      reveal: this._reveal,
      winner: ({ n }) => this._setWinner(n),
      idle: this._idle,
      admit: ({ mac, n }) => this._admitAction(mac, n),
      renumber: ({ n, to }) => this._renumber(n, to),
      release: ({ n }) => this._release(n),
      end: this._end,
      'usb-number': ({ n }) => this._usbNumber(n),
      'admit-mode': ({ mode }) => this._admitMode(mode),
    };
  }

  async _next() {
    if (!this.session) return this._startSession();
    this._needSession();
    const r = this.round;
    if (this.session.login) {
      if (this._nextDef()) return this._announce();
      return this._setLogin(false);
    }
    if (r && r.type !== 'idle' && !r.adopted) {
      if (r.phase === 'announced') return this._open();
      if (r.phase === 'open') return this._close();
      if (r.type === 'choice' && r.correct && !r.revealed) return this._reveal();
    }
    if (this._nextDef()) return this._announce();
    if (r && r.type !== 'idle') {
      await this._idle();
      return 'Keine weitere Runde geplant. Ruhe.';
    }
    throw new ActionError('Keine weitere Runde geplant. Neue Runden kommen über den Skill.');
  }

  async _startSession() {
    if (this.session) throw new ActionError('Es läuft schon eine Sitzung. Erst beenden.');
    this.session = {
      id: 1 + Math.floor(this.random() * 65535),
      channel: this.settings.channel,
      fleet: this.settings.fleet,
      login: true,
      ending: false,
    };
    this.devices.clear();
    this.released.clear();
    this.round = null;
    this.prevRound = null;
    this.lastPressed = null;
    this.ended = false;
    this.warning = null;
    this.plan.next = 0;
    this.plan.insertAt = 0;
    this._pushSession();
    const msg = `Sitzung ${this.session.id} gestartet, Kanal ${this.session.channel}. Anmeldung offen.`;
    this._msg('info', msg);
    return msg;
  }

  _setLogin(open) {
    this._needSession();
    if (this.session.login === open) return open ? 'Anmeldung ist schon offen.' : 'Anmeldung ist schon zu.';
    this.session.login = open;
    this._pushSession();
    const msg = open ? 'Anmeldung offen.' : 'Anmeldung geschlossen.';
    this._msg('info', msg);
    return msg;
  }

  async _announce(id) {
    this._needSession();
    let index;
    if (id !== undefined && id !== null && id !== '') {
      index = this.plan.rounds.findIndex((r) => r.id === id);
      if (index < 0) throw new ActionError(`Keine Runde mit der id "${id}" geladen.`);
    } else {
      index = this.plan.next;
      if (index >= this.plan.rounds.length) throw new ActionError('Keine weitere Runde geplant.');
    }
    const def = this.plan.rounds[index];
    await this._startRun(def);
    // The plan may have changed during the Nachlauf (input from the skill).
    const now = this.plan.rounds.indexOf(def);
    if (now >= 0) this.plan.next = now + 1;
    this.plan.insertAt = this.plan.next;
    const msg = def.type === 'idle'
      ? `Ruhe: ${def.title || def.id}.`
      : `Angekündigt: ${def.title || def.id}. Tasten gesperrt.`;
    this._msg('info', msg);
    return msg;
  }

  async _idle() {
    this._needSession();
    await this._startRun(null);
    return 'Ruhe.';
  }

  // Starts a new round (one start of a definition, or plain idle): closes a
  // round that is still open, keeps the 1 s Nachlauf, closes login.
  async _startRun(def) {
    if (this.round && this.round.phase === 'open') this._closeRound();
    const wait = this.lastCloseAt + TIMING.afterCloseMs - this.now();
    if (wait > 0) await new Promise((resolve) => setTimeout(resolve, wait));
    this._needSession();
    if (this.session.login) {
      this.session.login = false;
      this._pushSession();
    }
    clearTimeout(this.winnerTimer);
    const type = def ? def.type : 'idle';
    const run = {
      roundId: this._takeRoundId(),
      def,
      type,
      title: def ? def.title : '',
      question: def ? def.question : '',
      phase: type === 'idle' ? null : 'announced',
      keys: keysFor(type, def && def.answers ? def.answers.length : 0),
      values: new Map(),
      winner: 0,
      winnerManual: false,
      revealed: false,
      startAt: null,
      firstPressAt: null,
      winnerDue: false,
      invalid: false,
      adopted: false,
    };
    if (type === 'scale') Object.assign(run, { min: def.min, max: def.max, redMax: def.redMax, greenMin: def.greenMin });
    if (type === 'choice') Object.assign(run, { answers: def.answers, correct: def.correct });
    this.prevRound = this.round;
    this.round = run;
    this._pushRound();
    this._changed();
  }

  _open() {
    const r = this._needRound();
    if (r.phase !== 'announced') throw new ActionError('Öffnen geht nur bei einer angekündigten Runde.');
    r.phase = 'open';
    const openInMs = r.type === 'race' ? TIMING.raceOpenInMs : 0;
    r.startAt = this.now() + openInMs;
    this._pushRound({ openInMs });
    const msg = r.type === 'race' ? 'Rennen: Freigabe in 1 s.' : 'Runde offen.';
    this._msg('info', msg);
    return msg;
  }

  _close() {
    const r = this._needRound();
    if (r.phase !== 'open') throw new ActionError('Schließen geht nur bei einer offenen Runde.');
    this._closeRound();
    this._msg('info', 'Runde geschlossen.');
    return 'Runde geschlossen.';
  }

  _closeRound() {
    const r = this.round;
    r.phase = 'closed';
    r.closedAt = this.now();
    this.lastCloseAt = r.closedAt;
    this._pushRound();
  }

  _reveal() {
    const r = this._needRound();
    if (r.type !== 'choice') throw new ActionError('Auflösen geht nur bei einer Auswahl-Runde.');
    if (!r.correct) throw new ActionError('Diese Auswahl ist eine Umfrage ohne richtige Antwort.');
    if (r.phase === 'announced') throw new ActionError('Die Runde ist noch nicht offen.');
    if (r.revealed) return 'Schon aufgelöst.';
    if (r.phase === 'open') {
      r.phase = 'closed';
      r.closedAt = this.now();
      this.lastCloseAt = r.closedAt;
    }
    r.revealed = true;
    this._pushRound();
    const msg = `Aufgelöst: Antwort ${r.correct}.`;
    this._msg('info', msg);
    return msg;
  }

  _setWinner(n) {
    const r = this._needRound();
    if (r.type !== 'race') throw new ActionError('Einen Sieger gibt es nur beim Buzzer-Rennen.');
    const num = Number(n);
    if (!Number.isInteger(num) || num < 0 || num > MAX_NUMBER) throw new ActionError('Sieger: Nummer 1-999, 0 = keiner.');
    if (r.phase === 'announced') throw new ActionError('Das Rennen ist noch nicht offen.');
    r.winner = num;
    r.winnerManual = true;
    this._pushRound();
    const msg = num ? `Sieger von Hand gesetzt: ${num}.` : 'Sieger entfernt.';
    this._msg('info', msg);
    return msg;
  }

  _end() {
    this._needSession();
    if (this.session.ending) return 'Sitzung endet schon.';
    this.session.ending = true;
    this.session.login = false;
    clearTimeout(this.winnerTimer);
    this._pushEnd();
    const msg = 'Sitzung endet: das Gateway gibt alle Geräte 5 s lang frei.';
    this._msg('info', msg);
    return msg;
  }

  _admitMode(mode) {
    if (mode !== 'auto' && mode !== 'manual') throw new ActionError('Aufnahme-Modus: "auto" oder "manual".');
    this.settings.autoAdmit = mode === 'auto';
    const msg = this.settings.autoAdmit ? 'Geräte werden automatisch aufgenommen.' : 'Geräte werden nur von Hand aufgenommen.';
    this._msg('info', msg);
    if (this.settings.autoAdmit && this.session && this.session.login) {
      for (const d of this.devices.values()) if (d.waiting) this._admit(d);
    }
    return msg;
  }

  _admitAction(mac, n) {
    this._needSession();
    this._needLink();
    if (mac === 'all' || mac === undefined) {
      const waiting = [...this.devices.values()].filter((d) => d.waiting);
      for (const d of waiting) this._admit(d);
      return `${waiting.length} Gerät(e) werden aufgenommen.`;
    }
    const m = normalizeMac(mac);
    if (!m) throw new ActionError(`"${mac}" ist keine MAC-Adresse wie 02:00:00:00:00:11.`);
    const d = this.devices.get(m);
    if (!d) throw new ActionError(`Kein Gerät mit der MAC ${m} bekannt.`);
    let num;
    if (n !== undefined && n !== null) {
      num = this._checkFreeNumber(n, d);
    }
    this._admit(d, num);
    const msg = `Gerät ${m} wird mit Nummer ${d.n} aufgenommen.`;
    this._msg('info', msg);
    return msg;
  }

  _renumber(n, to) {
    this._needSession();
    this._needLink();
    const d = this._deviceByNumber(n);
    const num = this._checkFreeNumber(to, d);
    this._admit(d, num);
    const msg = `Gerät ${n} bekommt die Nummer ${num}.`;
    this._msg('info', msg);
    return msg;
  }

  _release(n) {
    this._needSession();
    const d = this._deviceByNumber(n);
    this.devices.delete(d.mac);
    this.released.set(d.mac, this.now());
    if (this.link.connected) {
      this.link.request(cmd.release(d.mac)).catch((err) => this._msg('error', `Freigabe ${n}: ${err.message}`));
    }
    const msg = `Gerät ${n} freigegeben.`;
    this._msg('info', msg);
    this._changed();
    return msg;
  }

  async _usbNumber(n) {
    this._needLink();
    let value;
    if (n === undefined) value = undefined;
    else if (n === null || n === 'none' || n === '') value = null;
    else {
      value = Number(n);
      if (!Number.isInteger(value) || value < 1 || value > MAX_NUMBER) throw new ActionError('Nummer 1-999 oder "none".');
    }
    try {
      const ev = await this.link.request(cmd.number(value));
      return ev.n ? `Das Gerät am USB-Kabel hat die Nummer ${ev.n}.` : 'Das Gerät am USB-Kabel hat keine Nummer.';
    } catch (err) {
      throw new ActionError(`Nummer am USB-Gerät: ${err.message}`);
    }
  }

  // ── Participants and numbers ─────────────────────────────────────────────
  _deviceByNumber(n) {
    const num = Number(n);
    for (const d of this.devices.values()) if (d.n === num && (d.admitted || d.admitting)) return d;
    throw new ActionError(`Kein Teilnehmer mit der Nummer ${n}.`);
  }

  _checkFreeNumber(n, self) {
    const num = Number(n);
    if (!Number.isInteger(num) || num < 1 || num > MAX_NUMBER) throw new ActionError('Nummern gehen von 1 bis 999.');
    for (const d of this.devices.values()) {
      if (d !== self && d.n === num) throw new ActionError(`Die Nummer ${num} hat schon ein anderes Gerät.`);
    }
    return num;
  }

  _numberFor(dev) {
    const taken = new Set();
    for (const d of this.devices.values()) if (d !== dev && d.n) taken.add(d.n);
    for (const want of [dev.n, dev.storedN]) {
      if (want && want >= 1 && want <= MAX_NUMBER && !taken.has(want)) return want;
    }
    for (let k = 1; k <= MAX_NUMBER; k++) if (!taken.has(k)) return k;
    return null;
  }

  _device(mac) {
    let d = this.devices.get(mac);
    if (!d) {
      d = {
        mac, n: null, storedN: null, admitted: false, admitting: false, waiting: false,
        fw: null, proto: null, bat: null, chg: false, rssi: null, lastSeen: 0, pressedAt: 0,
        lastRound: null, seq: null,
      };
      this.devices.set(mac, d);
    }
    return d;
  }

  _admit(dev, number) {
    if (!this.session || this.session.ending) return;
    const n = number ?? this._numberFor(dev);
    if (!n) {
      this._msg('error', 'Keine freie Nummer mehr (1-999).');
      return;
    }
    if (!this.link.connected) {
      this._msg('warn', `Aufnahme von ${dev.mac}: Gateway nicht verbunden.`);
      return;
    }
    dev.n = n;
    dev.waiting = false;
    dev.admitting = true;
    this.link.request(cmd.admit(dev.mac, n), { timeoutMs: 7000, retries: 0 })
      .then((ev) => {
        dev.admitting = false;
        if (this.devices.get(dev.mac) !== dev) return;
        dev.admitted = true;
        dev.n = ev.n;
        dev.lastSeen = this.now();
        this._changed();
      })
      .catch((err) => {
        dev.admitting = false;
        if (err.code === 'admit_timeout') this._msg('warn', `Gerät ${n} (${dev.mac}) hat die Aufnahme nicht bestätigt.`);
        else if (err.code !== 'offline' && err.code !== 'timeout') this._msg('error', `Aufnahme ${n}: ${err.message}`);
        this._changed();
      });
    this._changed();
  }

  // ── Gateway events ───────────────────────────────────────────────────────
  _onHello(ev) {
    if (!ev.spaces || ev.spaces.game !== 0) {
      this._msg('error', `Gateway spricht game-Version ${ev.spaces && ev.spaces.game}, die Konsole 0.`);
    }
    this._msg('info', `Gateway verbunden (fw ${ev.fw}, ${ev.mac}).`);
  }

  _onEvent(ev) {
    if (ev.ns === 'link' && ev.ev === 'error') {
      if (ev.id === undefined) this._msg('error', `Gateway meldet ${ev.code}: ${ev.msg || ''}`);
      else if (!['admit_timeout'].includes(ev.code)) this._msg('error', `Gateway lehnt ${ev.refNs || ''} ${ev.refCmd || ''} ab: ${ev.code}${ev.field ? ` (${ev.field})` : ''} ${ev.msg || ''}`);
      return;
    }
    if (ev.ns !== 'game') return;
    switch (ev.ev) {
      case 'session': return this._onSessionEv(ev);
      case 'state': return this._onStateEv(ev);
      case 'join': return this._onJoin(ev);
      case 'device': return this._onDevice(ev);
      case 'value': return this._onValue(ev);
      case 'admitted': return this._changed();
      default: return undefined;
    }
  }

  _onSessionEv(ev) {
    const s = this.session;
    if (!ev.active) {
      if (s && s.ending) return this._sessionEnded();
      if (s && ev.id === undefined) {
        // Replay after hello without a session: the gateway restarted.
        this.warning = 'Gateway neu gestartet: Sitzung und Runde wurden neu gesendet.';
        if (this.round && this.round.type === 'race' && this.round.phase !== 'announced') {
          this.round.invalid = true;
          this.warning = 'Gateway neu gestartet: die Rangliste dieses Rennens gilt nicht.';
        }
        this._msg('warn', this.warning);
        this._pushSession();
        this._pushRound();
      }
      return this._changed();
    }
    if (ev.id !== undefined) return this._changed();
    if (!s || s.id !== ev.session) {
      // The console started without this session (or knew another one):
      // the devices belong to the gateway's session, so take it over.
      this.session = { id: ev.session, channel: ev.channel, fleet: ev.fleet, login: ev.login, ending: false };
      this.devices.clear();
      this.round = null;
      this.prevRound = null;
      this._msg('warn', `Laufende Sitzung ${ev.session} vom Gateway übernommen.`);
      return this._changed();
    }
    if (s.ending) {
      this._pushEnd();
    } else if (s.login !== ev.login || s.channel !== ev.channel || s.fleet !== ev.fleet) {
      this._pushSession();
    }
    return this._changed();
  }

  _onStateEv(ev) {
    const r = this.round;
    if (ev.id !== undefined) {
      if (r && r.roundId === ev.round && typeof ev.sinceOpenMs === 'number' && ev.phase === 'open' && r.type === 'race') {
        // The gateway's clock decides when the race starts.
        r.startAt = this.now() - ev.sinceOpenMs;
      }
      return this._changed();
    }
    if (!this.session || this.session.ending) return undefined;
    if (!r) {
      if (ev.round !== 0 && ev.type !== 'idle') {
        this.round = {
          roundId: ev.round, def: null, type: ev.type, title: 'Laufende Runde', question: '',
          phase: ev.phase, keys: null, values: new Map(), winner: 0, winnerManual: true, revealed: false,
          startAt: typeof ev.sinceOpenMs === 'number' ? this.now() - ev.sinceOpenMs : null,
          firstPressAt: null, winnerDue: false, invalid: false, adopted: true,
          min: 0, max: 9, redMax: 3, greenMin: 7, answers: [], correct: null,
        };
        this._msg('warn', 'Laufende Runde vom Gateway übernommen; Titel und Frage sind unbekannt.');
      }
      return this._changed();
    }
    if (r.adopted) return this._changed();
    if (ev.round !== r.roundId || (r.type !== 'idle' && ev.phase !== r.phase)) this._pushRound();
    return this._changed();
  }

  _onJoin(ev) {
    const mac = normalizeMac(ev.mac);
    if (!mac || this._justReleased(mac)) return;
    const d = this._device(mac);
    this._status(d, ev);
    d.storedN = typeof ev.n === 'number' ? ev.n : null;
    if (ev.pressed) {
      d.pressedAt = this.now();
      this.lastPressed = { mac };
    }
    const s = this.session;
    if (!s || s.ending || d.admitting) return this._changed();
    if (d.admitted || ev.rejoin) {
      // A member pressing the buzzer while login is open, a device
      // rejoining after a restart, or one that lost its admission: admit
      // again with its number, no presenter needed. Members just confirm.
      this._admit(d);
    } else if (s.login && this.settings.autoAdmit) {
      this._admit(d);
    } else if (s.login) {
      if (!d.waiting) this._msg('info', `Gerät ${d.storedN ?? '?'} (${mac}) wartet auf Aufnahme.`);
      d.waiting = true;
    }
    return this._changed();
  }

  _onDevice(ev) {
    const mac = normalizeMac(ev.mac);
    if (!mac || !this.session || this._justReleased(mac)) return;
    const d = this._device(mac);
    this._status(d, ev);
    d.lastRound = ev.round;
    d.seq = ev.seq;
    if (!d.admitting) {
      // A status shows membership with this number; the `admitted` line
      // may have been dropped from the gateway's event queue.
      for (const other of this.devices.values()) {
        if (other !== d && other.n === ev.n) {
          other.n = null;
          other.admitted = false;
        }
      }
      d.n = ev.n;
    }
    if (d.n === ev.n) {
      d.admitted = true;
      d.waiting = false;
    }
    this._changed();
  }

  // The gateway repeats a single release for 2 s; the device may report
  // itself once more before it hears it.
  _justReleased(mac) {
    const at = this.released.get(mac);
    return at !== undefined && this.now() - at < RELEASE_GRACE_MS;
  }

  _status(d, ev) {
    d.lastSeen = this.now();
    if (ev.fw !== undefined) d.fw = ev.fw;
    if (ev.proto !== undefined) d.proto = ev.proto;
    if (ev.bat !== undefined) d.bat = ev.bat;
    if (ev.chg !== undefined) d.chg = ev.chg;
    if (ev.rssi !== undefined) d.rssi = ev.rssi;
  }

  _onValue(ev) {
    for (const d of this.devices.values()) {
      if (d.n === ev.n) {
        d.lastSeen = this.now();
        if (ev.bat !== undefined && ev.bat !== null) d.bat = ev.bat;
        if (ev.rssi !== undefined) d.rssi = ev.rssi;
      }
    }
    let run = null;
    if (this.round && this.round.roundId === ev.round) run = this.round;
    else if (this.prevRound && this.prevRound.roundId === ev.round) run = this.prevRound;
    if (!run || run.type === 'idle') return this._changed();
    // Every line is the gateway's newest value for this device. No vseq
    // filter: a device restarts its vseq after a rejoin.
    run.values.set(ev.n, { v: ev.v, t: ev.t, arr: ev.arr, late: !!ev.late, vseq: ev.vseq, at: this.now() });
    if (run.type === 'race' && run === this.round) this._raceUpdate(run);
    return this._changed();
  }

  _raceUpdate(run) {
    const any = [...run.values.values()].some((v) => !v.late);
    if (!any) return;
    if (!run.firstPressAt) {
      run.firstPressAt = this.now();
      clearTimeout(this.winnerTimer);
      this.winnerTimer = setTimeout(() => {
        run.winnerDue = true;
        this._autoWinner(run);
      }, TIMING.raceWinnerMs);
    }
    if (run.winnerDue) this._autoWinner(run);
  }

  _autoWinner(run) {
    if (run !== this.round || run.winnerManual) return;
    const first = raceRanking(run)[0];
    if (first && first.n !== run.winner) {
      const corrected = run.winner !== 0;
      run.winner = first.n;
      this._pushRound();
      if (corrected) this._msg('warn', `Sieger korrigiert: ${first.n} (früherer Druck kam spät an).`);
      this._changed();
    }
  }

  _sessionEnded() {
    this.session = null;
    this.round = null;
    this.prevRound = null;
    this.devices.clear();
    this.lastPressed = null;
    this.ended = true;
    clearTimeout(this.winnerTimer);
    this._msg('info', 'Sitzung beendet. Das Gateway bleibt bis zu seinem Neustart im Spielmodus.');
    this._changed();
  }

  // ── Sending state to the gateway ─────────────────────────────────────────
  _pushSession() {
    const s = this.session;
    if (!s || !this.link.connected) return;
    this.link.request(cmd.session({ session: s.id, channel: s.channel, fleet: s.fleet, login: s.login }))
      .catch((err) => {
        if (err.code === 'offline') return;
        this._msg('error', `Sitzung: ${err.message}`);
      });
  }

  _pushRound({ openInMs } = {}) {
    const r = this.round;
    if (!r || r.adopted || !this.session || this.session.ending || !this.link.connected) return;
    this.link.request(cmd.round(r, { openInMs }))
      .catch((err) => {
        if (err.code === 'offline') return;
        this._msg('error', `Runde: ${err.message}`);
      });
  }

  _pushEnd() {
    if (!this.link.connected) return;
    this.link.request(cmd.end(), { timeoutMs: 8000, retries: 0 })
      .catch((err) => {
        if (err.code === 'no_session') this._sessionEnded();
        else if (err.code !== 'offline') this._msg('error', `Sitzungsende: ${err.message}`);
      });
  }

  // ── Helpers ──────────────────────────────────────────────────────────────
  _needSession() {
    if (!this.session) throw new ActionError('Keine Sitzung. Erst starten.');
    if (this.session.ending) throw new ActionError('Die Sitzung endet gerade.');
  }

  _needLink() {
    if (!this.link.connected) throw new ActionError('Gateway nicht verbunden.');
  }

  _needRound() {
    this._needSession();
    const r = this.round;
    if (!r || r.type === 'idle') throw new ActionError('Keine Runde aktiv.');
    if (r.adopted) throw new ActionError('Die übernommene Runde lässt sich nicht steuern. Mit "Weiter" die nächste Runde ankündigen.');
    return r;
  }

  _nextDef() {
    return this.plan.rounds[this.plan.next] ?? null;
  }

  _takeRoundId() {
    const id = this.roundIdNext;
    this.roundIdNext = id >= 65535 ? 1 : id + 1;
    return id;
  }

  // A line for the presenter strip and the terminal.
  note(level, text) {
    this._msg(level, text);
  }

  _msg(level, text) {
    this.messages.push({ at: this.now(), level, text });
    if (this.messages.length > MAX_MESSAGES) this.messages.shift();
    this.emit('message', { level, text });
    this._changed();
  }

  _changed() {
    this.emit('change');
  }

  // ── View for the screen and /api/state ───────────────────────────────────
  snapshot() {
    const now = this.now();
    const link = this.link;
    const participants = [...this.devices.values()]
      .filter((d) => d.n && (d.admitted || d.admitting))
      .sort((a, b) => a.n - b.n)
      .map((d) => ({
        n: d.n, mac: d.mac, admitted: d.admitted, online: d.admitted && now - d.lastSeen < TIMING.offlineMs,
        bat: d.bat, chg: d.chg, rssi: d.rssi, fw: d.fw,
        pressed: !!this.lastPressed && this.lastPressed.mac === d.mac && now - d.pressedAt < PRESS_HIGHLIGHT_MS,
      }));
    const waiting = [...this.devices.values()]
      .filter((d) => d.waiting)
      .map((d) => ({ mac: d.mac, n: d.storedN, pressed: now - d.pressedAt < PRESS_HIGHLIGHT_MS }));
    const admitted = participants.filter((p) => p.admitted);
    let lastPressed = null;
    if (this.lastPressed) {
      const d = this.devices.get(this.lastPressed.mac);
      if (d && now - d.pressedAt < PRESS_HIGHLIGHT_MS) lastPressed = { n: d.n ?? d.storedN, mac: d.mac };
    }
    const s = this.session;
    const r = this.round;
    let view = 'start';
    if (s && s.ending) view = 'idle';
    else if (s && s.login) view = 'login';
    else if (s && r && r.type !== 'idle') view = r.type;
    else if (s) view = 'idle';
    const next = this._nextDef();
    return {
      now,
      view,
      link: {
        mode: this.mode,
        state: link.state,
        port: link.port ? link.port.path : null,
        status: link.transportStatus,
        fw: link.hello ? link.hello.fw : null,
        mac: link.hello ? link.hello.mac : null,
        counters: link.counters,
      },
      settings: { ...this.settings },
      session: s ? { id: s.id, channel: s.channel, fleet: s.fleet, login: s.login, ending: s.ending } : null,
      ended: this.ended,
      title: this.plan.title,
      plan: {
        count: this.plan.rounds.length,
        next: next ? { id: next.id, title: next.title, type: next.type } : null,
        rounds: this.plan.rounds.map((d, i) => ({ id: d.id, title: d.title, type: d.type, done: i < this.plan.next })),
      },
      participants,
      waiting,
      counts: { admitted: admitted.length, online: admitted.filter((p) => p.online).length, waiting: waiting.length },
      lastPressed,
      round: r ? roundView(r, admitted.length) : null,
      messages: this.messages.slice(-5),
      warning: this.warning,
    };
  }
}

export function zoneOf(r, v) {
  if (v <= r.redMax) return 'red';
  if (v >= r.greenMin) return 'green';
  return 'yellow';
}

// Ranking by the reported press moment; arrival order breaks exact ties.
// A press less than 5 ms behind the first press of its rank group shares
// that rank and is marked as a tie.
export function raceRanking(run) {
  const entries = [...run.values.entries()]
    .filter(([, v]) => !v.late)
    .map(([n, v]) => ({ n, t: v.t, arr: v.arr, rank: 0, tie: false }))
    .sort((a, b) => a.t - b.t || a.arr - b.arr);
  let head = null;
  entries.forEach((e, i) => {
    if (head && e.t - head.t < TIMING.tieMs) {
      e.rank = head.rank;
      e.tie = true;
      head.tie = true;
    } else {
      e.rank = i + 1;
      head = e;
    }
  });
  return entries;
}

function roundView(r, participants) {
  const base = {
    roundId: r.roundId,
    id: r.def ? r.def.id : null,
    title: r.title,
    question: r.question,
    type: r.type,
    phase: r.phase,
    adopted: r.adopted,
    startAt: r.startAt,
  };
  const valid = [...r.values.entries()].filter(([, v]) => !v.late);
  if (r.type === 'scale') {
    const entries = valid.map(([n, v]) => ({ n, v: v.v, zone: zoneOf(r, v.v) })).sort((a, b) => a.n - b.n);
    const counts = [];
    for (let v = r.min; v <= r.max; v++) counts.push({ v, zone: zoneOf(r, v), count: entries.filter((e) => e.v === v).length });
    const mean = entries.length ? entries.reduce((s, e) => s + e.v, 0) / entries.length : null;
    return { ...base, min: r.min, max: r.max, redMax: r.redMax, greenMin: r.greenMin, results: { entries, counts, mean, voters: entries.length, participants } };
  }
  if (r.type === 'race') {
    return { ...base, winner: r.winner || null, invalid: r.invalid, results: { ranking: raceRanking(r), participants } };
  }
  if (r.type === 'choice') {
    const answered = valid.length;
    const results = { answered, participants };
    if (r.phase === 'closed') {
      results.counts = r.answers.map((_, i) => valid.filter(([, v]) => v.v === i + 1).length);
      if (r.answers.length === 0) results.counts = [1, 2, 3, 4].map((a) => valid.filter(([, v]) => v.v === a).length);
    }
    if (r.revealed) results.right = valid.filter(([, v]) => v.v === r.correct).map(([n]) => n).sort((a, b) => a - b);
    return { ...base, answers: r.answers, correct: r.revealed ? r.correct : null, hasCorrect: !!r.correct, revealed: r.revealed, results };
  }
  return base;
}
