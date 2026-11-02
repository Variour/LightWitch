import { EventEmitter } from 'node:events';
import { cmd, keysFor, normalizeMac, FW_RE, seq8Newer } from './protocol.js';
import { FleetTable } from './fleet.js';
import { boardDirection, startCell, step, inside, atGoal, groupOf, teamStart, MOVE_DEFAULTS } from './move.js';
import { isLage, requiredLage, fitsLage } from './lage.js';

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
const FW_CHANGE_SHOW_MS = 60000; // the screen names a new gateway build this long
const TARGET_NAMES = { store: 'Lagern (80 %)', event: 'Event (100 %)' };
const SSID_MAX_BYTES = 32;

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
    this.fleet = new FleetTable({ now });
    this.fleetView = false; // screen shows the charging overview
    this.fwRef = null; // reference firmware set in the console, else the gateway's
    this.gatewayFw = null; // build of the last hello
    this.gatewayFwChange = null; // { from, to, at } after a hello with another build

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
      'fleet-view': ({ mode } = {}) => this._fleetView(mode),
      'update-all': ({ pr } = {}) => this._updateAll(pr),
      charge: ({ target } = {}) => this._charge(target),
      'off-all': this._offAll,
      ap: ({ to } = {}) => this._ap(to),
      'wifi-set': ({ ssid, pass } = {}) => this._wifiSet(ssid, pass),
      'config-get': this._configGet,
      'fw-ref': ({ fw } = {}) => this._setFwRef(fw),
      group: ({ k } = {}) => this._setGroup(k),
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
    if (type !== 'idle') {
      // lageAtOpen: n -> lage at the open, the round's lage with hold "fest".
      Object.assign(run, { hold: def.hold, lageAtOpen: new Map() });
    }
    if (type === 'scale') Object.assign(run, { min: def.min, max: def.max, redMax: def.redMax, greenMin: def.greenMin });
    if (type === 'choice') Object.assign(run, { answers: def.answers, correct: def.correct });
    if (type === 'move') {
      // pos: n -> { col, row }; order: numbers in the goal (team: team
      // numbers), first is the winner; kseq: the last press counter applied
      // per number; teams: k -> { x, y } (team); chosen: numbers with a
      // place, in order (erste); group: the active group (gruppen).
      Object.assign(run, {
        goal: def.goal, cols: def.cols, rows: def.rows, play: def.play,
        pos: new Map(), order: [], kseq: new Map(), teams: new Map(), chosen: [], group: null,
      });
    }
    this.prevRound = this.round;
    this.round = run;
    this._pushRound();
    this._changed();
  }

  _open() {
    const r = this._needRound();
    if (r.phase !== 'announced') throw new ActionError('Öffnen geht nur bei einer angekündigten Runde.');
    r.phase = 'open';
    r.lageAtOpen.clear();
    for (const d of this.devices.values()) if (d.n && d.admitted) r.lageAtOpen.set(d.n, d.lage);
    if (r.type === 'move') {
      // Everybody back to the start cell; _changed() places them.
      r.pos.clear();
      r.teams.clear();
      r.order = [];
      r.chosen = [];
      if (r.play === 'gruppen') r.group = this._groups()[0] ?? null;
    }
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

  // Move rounds with "play": "gruppen": the group that moves now.
  _setGroup(k) {
    const r = this._needRound();
    if (r.type !== 'move' || r.play !== 'gruppen') throw new ActionError('Eine Gruppe wählen geht nur bei einer Spielfeld-Runde mit "play": "gruppen".');
    if (r.phase !== 'open') throw new ActionError('Eine Gruppe wählen geht nur, solange die Runde offen ist.');
    const num = Number(k);
    if (k === undefined || k === null || k === '' || !Number.isInteger(num) || num < 1) throw new ActionError('Gruppe: ganze Zahl ab 1.');
    if (!this._groups().includes(num)) throw new ActionError(`Gruppe ${num} hat keine Teilnehmer.`);
    r.group = num;
    const msg = `Gruppe ${num} ist dran.`;
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

  // ── Fleet and config (fleet protocol v0 draft) ───────────────────────────
  _fleetView(mode) {
    if (mode !== undefined && mode !== 'on' && mode !== 'off') throw new ActionError('Ladeübersicht: "on" oder "off".');
    this.fleetView = mode === undefined ? !this.fleetView : mode === 'on';
    this._changed();
    return this.fleetView ? 'Ladeübersicht an.' : 'Ladeübersicht aus.';
  }

  // The gateway names every namespace it speaks in its hello.
  _needSpace(ns, what) {
    this._needLink();
    const spaces = this.link.hello && this.link.hello.spaces;
    const v = spaces ? spaces[ns] : undefined;
    if (v !== 0) {
      const fw = this.link.hello && this.link.hello.fw ? `, Firmware ${this.link.hello.fw}` : '';
      throw new ActionError(`Das Gerät am USB-Kabel kennt ${what} nicht (Namensraum "${ns}" Version 0 fehlt im hello${fw}).`);
    }
  }

  async _fleetSend(command, what) {
    this._needSpace('fleet', 'die Flottenbefehle');
    try {
      await this.link.request(command);
    } catch (err) {
      throw new ActionError(`${what}: ${err.message}`);
    }
    return this.session && !this.session.ending ? ' Geräte in der laufenden Sitzung ignorieren den Befehl.' : '';
  }

  async _updateAll(pr) {
    let num;
    if (pr !== undefined && pr !== null && pr !== '') {
      num = Number(pr);
      if (!Number.isInteger(num) || num < 1 || num > 65535) throw new ActionError('PR-Nummer: ganze Zahl von 1 bis 65535.');
    }
    const note = await this._fleetSend(cmd.fleetUpdate(num), 'Update an alle');
    const msg = `Update an alle gesendet (${num ? `PR ${num}` : 'neuestes Release'}). Das Gateway aktualisiert sich mit und startet bei einer neuen Firmware neu.${note}`;
    this._msg('info', msg);
    return msg;
  }

  async _charge(target) {
    if (!TARGET_NAMES[target]) throw new ActionError('Ladeziel: "store" (Lagern, 80 %) oder "event" (Event, 100 %).');
    const note = await this._fleetSend(cmd.fleetCharge(target), 'Ladeziel');
    const msg = `Ladeziel ${TARGET_NAMES[target]} an alle gesendet.${note}`;
    this._msg('info', msg);
    return msg;
  }

  async _offAll() {
    const note = await this._fleetSend(cmd.fleetOff(), 'Ausschalten');
    const gone = this.fleet.removeOnBattery();
    this._changed();
    const msg = `Ausschalten an alle gesendet. ${gone} Gerät(e) im Akkubetrieb aus der Ladeübersicht genommen, Geräte am USB-Strom bleiben an.${note}`;
    this._msg('info', msg);
    return msg;
  }

  async _ap(to) {
    let mac = normalizeMac(to);
    let label;
    if (mac) {
      const d = this.fleet.devices.get(mac);
      label = d && d.n ? `Gerät ${d.n} (${mac})` : `Gerät ${mac}`;
    } else {
      const n = Number(to);
      if (to === undefined || to === null || to === '' || !Number.isInteger(n) || n < 1 || n > MAX_NUMBER) {
        throw new ActionError('Wartungs-WLAN: Nummer 1-999 oder MAC wie 02:00:00:00:00:11 angeben.');
      }
      const found = this.fleet.byNumber(n);
      if (!found.length) throw new ActionError(`Kein Gerät mit der Nummer ${n} in der Ladeübersicht. Mit MAC angeben.`);
      if (found.length > 1) throw new ActionError(`Die Nummer ${n} haben mehrere Geräte: ${found.map((d) => d.mac).join(', ')}. Mit MAC angeben.`);
      mac = found[0].mac;
      label = `Gerät ${n} (${mac})`;
    }
    const note = await this._fleetSend(cmd.fleetAp(mac), 'Wartungs-WLAN');
    const msg = `Wartungs-WLAN für ${label} angefordert (10 Minuten).${note}`;
    this._msg('info', msg);
    return msg;
  }

  // The password goes to the device on the USB cable and nowhere else: not
  // into messages, not into the state, not into the trace.
  async _wifiSet(ssid, pass) {
    if (typeof ssid !== 'string' || !ssid.length || Buffer.byteLength(ssid, 'utf8') > SSID_MAX_BYTES) {
      throw new ActionError('WLAN-Name: 1 bis 32 Bytes.');
    }
    if (typeof pass !== 'string') throw new ActionError('WLAN-Passwort fehlt (leer für ein offenes Netz).');
    const printable = /^[\x20-\x7e]*$/.test(pass);
    if (pass.length && !(printable && pass.length >= 8 && pass.length <= 63)) {
      throw new ActionError('WLAN-Passwort: 8 bis 63 Zeichen (ohne Umlaute) oder leer für ein offenes Netz.');
    }
    this._needSpace('config', 'die Konfiguration über USB');
    let ev;
    try {
      ev = await this.link.request(cmd.configWifi([{ ssid, pass }]));
    } catch (err) {
      throw new ActionError(`WLAN setzen: ${err.message}`);
    }
    if (ev.ok === false) throw new ActionError('WLAN setzen: das Gerät hat abgelehnt.');
    const msg = `WLAN "${ssid}" auf dem Gerät am USB-Kabel gespeichert (${ev.count ?? 1} Netz).`;
    this._msg('info', msg);
    return msg;
  }

  async _configGet() {
    this._needSpace('config', 'die Konfiguration über USB');
    let ev;
    try {
      ev = await this.link.request(cmd.configGet());
    } catch (err) {
      throw new ActionError(`Konfiguration lesen: ${err.message}`);
    }
    const target = TARGET_NAMES[ev.target] ?? (ev.target ? String(ev.target) : '?');
    const number = Number.isInteger(ev.number) && ev.number > 0 ? ev.number : 'keine';
    return `Gerät am USB-Kabel: Firmware ${ev.fw ?? '?'}, Nummer ${number}, ${ev.wifiCount ?? '?'} WLAN-Netz(e), Ladeziel ${target}.`;
  }

  _setFwRef(fw) {
    if (fw === undefined || fw === null || fw === '' || fw === 'gateway') {
      this.fwRef = null;
      this._changed();
      const gw = this.link.hello && this.link.hello.fw;
      return `Referenz-Firmware ist die des Gateways${gw ? ` (${gw})` : ''}.`;
    }
    const v = String(fw).toLowerCase();
    if (!FW_RE.test(v)) throw new ActionError('Referenz-Firmware: 8 Hex-Zeichen wie 1a2b3c4d oder "gateway".');
    this.fwRef = v;
    this._changed();
    return `Referenz-Firmware ${v}.`;
  }

  _refFw() {
    if (this.fwRef) return { fw: this.fwRef, source: 'console' };
    const gw = this.link.hello && typeof this.link.hello.fw === 'string' ? this.link.hello.fw.toLowerCase() : null;
    return { fw: gw, source: gw ? 'gateway' : null };
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
        fw: null, proto: null, bat: null, chg: false, rssi: null, lage: null, lastSeen: 0, pressedAt: 0,
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
    const prev = this.gatewayFw;
    this.gatewayFw = ev.fw ?? null;
    if (prev && ev.fw && prev !== ev.fw) {
      // `update-all` updates the gateway too: it restarts with the new build.
      this.gatewayFwChange = { from: prev, to: ev.fw, at: this.now() };
      this._msg('info', `Gateway wieder verbunden, neue Firmware ${ev.fw} (vorher ${prev}).`);
      return;
    }
    this._msg('info', `Gateway verbunden (fw ${ev.fw}, ${ev.mac}).`);
  }

  _onEvent(ev) {
    if (ev.ns === 'link' && ev.ev === 'error') {
      if (ev.id === undefined) this._msg('error', `Gateway meldet ${ev.code}: ${ev.msg || ''}`);
      else if (!['admit_timeout'].includes(ev.code)) this._msg('error', `Gateway lehnt ${ev.refNs || ''} ${ev.refCmd || ''} ab: ${ev.code}${ev.field ? ` (${ev.field})` : ''} ${ev.msg || ''}`);
      return;
    }
    if (ev.ns === 'fleet') {
      if (ev.ev === 'status' && this.fleet.onStatus(ev)) this._changed();
      return;
    }
    if (ev.ns !== 'game') return;
    switch (ev.ev) {
      case 'session': return this._onSessionEv(ev);
      case 'state': return this._onStateEv(ev);
      case 'join': return this._onJoin(ev);
      case 'device': return this._onDevice(ev);
      case 'value': return this._onValue(ev);
      case 'key': return this._onKey(ev);
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
        this._resendAll();
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
          hold: 'frei', lageAtOpen: new Map(), ...MOVE_DEFAULTS, pos: new Map(), order: [], kseq: new Map(), teams: new Map(), chosen: [], group: null,
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
    const r = this.round;
    if (r && r.type === 'move') {
      // A joining device starts its press counter again.
      for (const n of [ev.n, d.n]) if (n) r.kseq.delete(n);
    }
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
    if (ev.lage !== undefined) d.lage = isLage(ev.lage) ? ev.lage : null;
  }

  // The lage a participant has for this round: with hold "fest" the one at
  // the open (a participant admitted later: the one when first asked), else
  // the newest from its status. null: unknown.
  _lageOf(run, n) {
    let live = null;
    for (const d of this.devices.values()) if (d.n === n && d.admitted) live = d.lage;
    if (run.hold !== 'fest' || run.phase === 'announced') return live;
    if (!run.lageAtOpen.has(n)) run.lageAtOpen.set(n, live);
    return run.lageAtOpen.get(n);
  }

  // An input made in another lage than the round requires does not count.
  // The input's own lage decides, else the participant's; unknown passes.
  _wrongLage(run, n, inputLage) {
    const need = requiredLage(run.hold);
    if (!need) return false;
    const lage = isLage(inputLage) ? inputLage : this._lageOf(run, n);
    return lage !== null && !fitsLage(lage, need);
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
    if (this._wrongLage(run, ev.n, ev.lage)) return this._changed();
    // Every line is the gateway's newest value for this device. No vseq
    // filter: a device restarts its vseq after a rejoin.
    run.values.set(ev.n, { v: ev.v, t: ev.t, arr: ev.arr, late: !!ev.late, vseq: ev.vseq, at: this.now() });
    if (run.type === 'race' && run === this.round) this._raceUpdate(run);
    return this._changed();
  }

  // One press of a device in a move round (provisional `game key` line).
  // A kseq that is not newer than the last applied one is a resend. With a
  // lage the key is logical, without one the physical button.
  _onKey(ev) {
    for (const d of this.devices.values()) if (d.n === ev.n) d.lastSeen = this.now();
    const r = this.round;
    if (!r || r.type !== 'move' || r.adopted || r.roundId !== ev.round || !Number.isInteger(ev.kseq)) return this._changed();
    const last = r.kseq.get(ev.n);
    if (last !== undefined && !seq8Newer(ev.kseq, last)) return this._changed();
    r.kseq.set(ev.n, ev.kseq);
    if (r.phase === 'open' && this._admittedNumbers().includes(ev.n) && !this._wrongLage(r, ev.n, ev.lage)) {
      this._moveSync();
      const dir = boardDirection(r.hold, ev.key, isLage(ev.lage) ? ev.lage : null);
      if (r.play === 'team') this._teamStep(r, groupOf(ev.n), dir);
      else if (r.play === 'erste' && !r.chosen.includes(ev.n)) {
        // The first press takes a place, if one is left, and does not move.
        if (r.chosen.length < r.rows) {
          r.chosen.push(ev.n);
          this._moveSync();
        }
      } else if (r.play !== 'gruppen' || groupOf(ev.n) === r.group) this._moveStep(r, ev.n, dir);
    }
    return this._changed();
  }

  // A press of a team member moves the team by 1/members of a cell. Teams
  // do not block each other; a team at the last column is in the goal.
  _teamStep(r, k, dir) {
    const t = r.teams.get(k);
    const members = this._admittedNumbers().filter((n) => groupOf(n) === k).length;
    if (!t || !dir || !members || r.order.includes(k)) return;
    const d = step({ col: 0, row: 0 }, dir);
    const clamp = (v, hi) => Math.min(Math.max(v, 0), hi);
    t.x = clamp(t.x + d.col / members, r.cols - 1);
    t.y = clamp(t.y + d.row / members, r.rows - 1);
    if (t.x >= r.cols - 1 - 1e-9) {
      t.x = r.cols - 1;
      r.order.push(k);
    }
  }

  // What participant n can do in the open move round: 'play', 'wait' (its
  // group is not active) or 'out' (in the goal, no place left, not taking
  // part). For the simulated people and the keyboard.
  moveRole(n) {
    const r = this.round;
    if (!r || r.type !== 'move' || r.adopted || r.phase !== 'open' || !this._admittedNumbers().includes(n)) return 'out';
    if (r.order.includes(r.play === 'team' ? groupOf(n) : n)) return 'out';
    if (r.play === 'erste') return r.chosen.includes(n) || r.chosen.length < r.rows ? 'play' : 'out';
    if (r.play === 'gruppen') return groupOf(n) === r.group ? 'play' : 'wait';
    return 'play';
  }

  // One cell in the mapped direction. Ignored past the border, onto an
  // occupied cell and for a device already in the goal.
  _moveStep(r, n, dir) {
    const from = r.pos.get(n);
    if (!from || !dir || r.order.includes(n)) return;
    const to = step(from, dir);
    if (!inside(to, r.cols, r.rows)) return;
    for (const c of r.pos.values()) if (c.col === to.col && c.row === to.row) return;
    r.pos.set(n, to);
    if (atGoal(to, r.goal, r.cols)) r.order.push(n);
  }

  // Keeps the board in step with the participants: one figure per admitted
  // participant, a newcomer on its start cell as soon as that is free. A
  // start cell beyond the board or in the goal column gives no figure.
  // Team: a figure for every group with a member, from its first member on.
  // Erste: figures only for the participants with a place, in col 0 by order.
  _moveSync() {
    const r = this.round;
    if (!r || r.type !== 'move' || r.adopted) return;
    const present = this._admittedNumbers();
    if (r.play === 'team') {
      for (const k of this._groups()) if (!r.teams.has(k)) r.teams.set(k, teamStart(k, r.rows));
      return;
    }
    if (r.play === 'gruppen' && r.phase === 'open' && r.group === null) r.group = this._groups()[0] ?? null;
    const playing = r.play === 'erste' ? r.chosen.filter((n) => present.includes(n)) : present;
    const keep = new Set(playing);
    for (const n of r.pos.keys()) if (!keep.has(n)) r.pos.delete(n);
    const cellKey = (c) => `${c.col},${c.row}`;
    const taken = new Set([...r.pos.values()].map(cellKey));
    for (const n of playing) {
      if (r.pos.has(n)) continue;
      const c = r.play === 'erste' ? { col: 0, row: r.chosen.indexOf(n) } : startCell(n, r.rows);
      if (!inside(c, r.cols, r.rows) || atGoal(c, r.goal, r.cols) || taken.has(cellKey(c))) continue;
      r.pos.set(n, c);
      taken.add(cellKey(c));
    }
  }

  // Admitted participants in the lage the round requires; null without one.
  _readyCount(r) {
    const need = r.type === 'idle' ? null : requiredLage(r.hold);
    if (!need) return null;
    return this._admittedNumbers().filter((n) => fitsLage(this._lageOf(r, n), need)).length;
  }

  _admittedNumbers() {
    const ns = [];
    for (const d of this.devices.values()) if (d.n && d.admitted) ns.push(d.n);
    return ns.sort((a, b) => a - b);
  }

  // Groups with at least one admitted member, ascending.
  _groups() {
    return [...new Set(this._admittedNumbers().map(groupOf))];
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

  // After a gateway restart: session and running round in one write. The
  // gateway reads both in the same pass and goes straight from no session
  // to the running round; between two passes it would send idle round 0,
  // and the devices would drop their shown value.
  _resendAll() {
    const s = this.session;
    const r = this.round;
    if (!s || s.ending || !this.link.connected) return;
    const commands = [cmd.session({ session: s.id, channel: s.channel, fleet: s.fleet, login: s.login })];
    if (r && !r.adopted) commands.push(cmd.round(r));
    const [session, round] = this.link.requestAll(commands);
    session.catch((err) => {
      if (err.code !== 'offline') this._msg('error', `Sitzung: ${err.message}`);
    });
    if (round) {
      round.catch((err) => {
        if (err.code !== 'offline') this._msg('error', `Runde: ${err.message}`);
      });
    }
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
    this._moveSync();
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
        bat: d.bat, chg: d.chg, rssi: d.rssi, fw: d.fw, lage: d.lage,
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
    if (this.fleetView) view = 'fleet';
    const next = this._nextDef();
    const ref = this._refFw();
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
        fwChange: this.gatewayFwChange && now - this.gatewayFwChange.at < FW_CHANGE_SHOW_MS ? { ...this.gatewayFwChange } : null,
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
      round: r ? roundView(r, admitted.map((p) => p.n), this._readyCount(r)) : null,
      messages: this.messages.slice(-5),
      warning: this.warning,
      fleet: { ref, ...this.fleet.snapshot(ref.fw) },
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

function roundView(r, admitted, ready) {
  const participants = admitted.length;
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
  if (r.type === 'idle') return base;
  base.hold = r.hold;
  const valid = [...r.values.entries()].filter(([, v]) => !v.late);
  if (r.type === 'scale') {
    const entries = valid.map(([n, v]) => ({ n, v: v.v, zone: zoneOf(r, v.v) })).sort((a, b) => a.n - b.n);
    const counts = [];
    for (let v = r.min; v <= r.max; v++) counts.push({ v, zone: zoneOf(r, v), count: entries.filter((e) => e.v === v).length });
    const mean = entries.length ? entries.reduce((s, e) => s + e.v, 0) / entries.length : null;
    return { ...base, min: r.min, max: r.max, redMax: r.redMax, greenMin: r.greenMin, results: { entries, counts, mean, voters: entries.length, participants, ready } };
  }
  if (r.type === 'race') {
    return { ...base, winner: r.winner || null, invalid: r.invalid, results: { ranking: raceRanking(r), participants, ready } };
  }
  if (r.type === 'choice') {
    const answered = valid.length;
    const results = { answered, participants, ready };
    if (r.phase === 'closed') {
      results.counts = r.answers.map((_, i) => valid.filter(([, v]) => v.v === i + 1).length);
      if (r.answers.length === 0) results.counts = [1, 2, 3, 4].map((a) => valid.filter(([, v]) => v.v === a).length);
    }
    if (r.revealed) results.right = valid.filter(([, v]) => v.v === r.correct).map(([n]) => n).sort((a, b) => a - b);
    return { ...base, answers: r.answers, correct: r.revealed ? r.correct : null, hasCorrect: !!r.correct, revealed: r.revealed, results };
  }
  if (r.type === 'move') {
    const team = r.play === 'team';
    const players = admitted.map((n) => {
      const c = team ? undefined : r.pos.get(n);
      const i = team ? -1 : r.order.indexOf(n);
      return { n, col: c ? c.col : null, row: c ? c.row : null, place: i >= 0 ? i + 1 : null };
    });
    const results = { players, order: [...r.order], winner: r.order[0] ?? null, finished: r.order.length, participants, ready, play: r.play };
    if (team) {
      const round3 = (v) => Math.round(v * 1000) / 1000;
      const members = (k) => admitted.filter((n) => groupOf(n) === k).length;
      results.teams = [...r.teams.entries()]
        .filter(([k]) => members(k) > 0)
        .sort((a, b) => a[0] - b[0])
        .map(([k, t]) => ({ team: k, members: members(k), x: round3(t.x), y: round3(t.y), place: r.order.includes(k) ? r.order.indexOf(k) + 1 : null }));
    } else if (r.play === 'erste') {
      results.chosen = [...r.chosen];
      results.watching = admitted.filter((n) => !r.chosen.includes(n)).length;
    } else if (r.play === 'gruppen') {
      results.group = r.group;
    }
    return { ...base, goal: r.goal, cols: r.cols, rows: r.rows, results };
  }
  return base;
}
