// Simulated Buzzerboards for the fake gateway. The device logic follows
// src/game/Participant.h on the game-session branch at 723472b (join, admit,
// echo, resend, status, rejoin after a restart, following the round across a
// bridge restart); a small "person" on top presses keys like a participant
// would. Power and fleet behaviour live in power.js. Move rounds and the
// orientation ("Lage") are provisional: the firmware has neither yet. The
// simulated buzzers already do what the firmware will: accept a new lage
// after 2 s of holding still, remap their buttons and report logical inputs
// with their lage, lock themselves in a round that requires another lage.

import { seq8Newer, seq16Newer } from '../protocol.js';
import { LAGEN, requiredLage, fitsLage, toLogical, toPhysical } from '../lage.js';
import { SimPower } from './power.js';

export const T = {
  joinRepeatMin: 300, joinRepeatMax: 600, // join request every 300-600 ms
  joinTimeout: 30000,
  inviteVisible: 10000,
  rejoinListen: 10000,
  statusPeriod: 15000, statusJitter: 1500,
  scaleSendDelay: 150,
  retryMin: 300, retryMax: 600, retryCap: 2000,
  noGatewaySearch: 10000,
  searchDwell: 600,
};

// Move rounds: each person presses at a pace of their own.
export const MOVE_T = {
  movePaceMin: 450, movePaceMax: 1000, // mean time between two presses
  moveFirstMin: 300, moveFirstMax: 1500, // first press after the open
};
const MOVE_LOG = 8; // unconfirmed presses a buzzer keeps and sends again

// Orientation: the buzzer takes a new lage after holding still this long. In a
// round that requires a lage, most people turn their buzzer after 1-6 s, a
// few much later, some never.
export const LAGE_T = {
  lageSettleMs: 2000,
  turnMin: 1000, turnMax: 6000,
  turnSlowMin: 15000, turnSlowMax: 45000,
  turnSlow: 0.06, turnNever: 0.04, // shares of the people
};
const LOCKED_RETRY_MS = 200; // a locked buzzer: the person tries again

const NO_NUMBER = 0xffff;
const CHOICE_KEYS = ['up', 'right', 'down', 'left'];

export class SimDevice {
  constructor(world, { index, mac, storedN, channel, rssi, power }) {
    this.world = world;
    this.index = index;
    this.mac = mac;
    this.number = storedN || 0;
    this.homeChannel = channel; // channel in normal operation
    this.channel = channel;
    this.rssiBase = rssi;
    this.state = 'normal'; // normal | booting | rejoinListen | joining | member
    this.session = 0;
    this.sessionChannel = 0;
    this.msgId = 0;
    this.invite = null; // { at, session, channel }
    this.joinPlanned = false;
    this.presses = []; // the person's planned presses { at, key: logical input }
    this.round = null;
    this._resetValue();
    this.lastGateway = 0;
    this.searching = false;
    this.manual = false; // the keyboard controls this buzzer, the person pauses
    this.physLage = world.lrnd.pick(LAGEN); // how the buzzer lies or is held
    this.lage = this.physLage; // the lage the buzzer has taken
    this.physAt = 0; // when it was turned last
    this.turn = null; // { at, to }: the person turns it then
    this.power = new SimPower(world, this, power);
  }

  get battery() {
    return Math.round(this.power.bat);
  }

  get rssi() {
    return this.rssiBase + this.world.rnd.int(-3, 3);
  }

  // ── Radio in ─────────────────────────────────────────────────────────────
  onFrame(f, now) {
    if (f.origin !== 0 || this.state === 'booting') return;
    if (f.type === 'fleetCommand') {
      if (this.state === 'normal') this.power.onCommand(f, now); // members ignore fleet commands
      return;
    }
    if (this.power.blocksGame(now)) return;
    if (this.state === 'normal') {
      if (f.type === 'invite' && (f.fleet === 0)) {
        this.power.wake();
        this.invite = { at: now, session: f.session, channel: f.channel };
        if (!this.joinPlanned && this.world.person.wantsToJoin(this)) {
          this.joinPlanned = true;
          this.presses.push({ at: now + this.world.rnd.int(300, 2500), key: 'buzz' });
        }
      }
      return;
    }
    if (f.session !== this.session) return;
    if (f.type === 'invite') this._follow(f.channel);
    if (f.type === 'release' && (f.mac === this.mac || f.mac === 'ff:ff:ff:ff:ff:ff')) {
      this._leave();
      return;
    }
    if (f.type === 'channel') {
      this.pendingChannel = f.channel;
      this.switchAt = now + f.switchInMs;
    }
    this.lastGateway = now;
    this.searching = false;
    if (this.state === 'rejoinListen') {
      this._startJoin(false, true, now);
    } else if (this.state === 'joining') {
      if (f.type === 'admit') this._onAdmit(f, now);
    } else if (this.state === 'member') {
      if (f.type === 'admit') this._onAdmit(f, now);
      else if (f.type === 'state') this._onState(f, now);
    }
  }

  _follow(ch) {
    if (ch !== this.channel) this.channel = ch;
    this.sessionChannel = ch;
  }

  _onAdmit(f, now) {
    if (f.mac !== this.mac || f.number < 1 || f.number > 999) return;
    this.number = f.number;
    if (this.state === 'joining') {
      this.state = 'member';
      this.round = null;
      this._resetValue();
    }
    this._sendStatus(0);
    this.nextStatusAt = now + this._statusInterval();
  }

  _onState(f, now) {
    // The bridge sends only its current state, in order, so any frame that
    // differs from the applied state is the newer one. A stateSeq that did
    // not move forward means the bridge restarted and the console resent the
    // round: its round clock restarted too.
    const newRound = !this.round || f.roundId !== this.round.id;
    const differs = newRound || !this._sameState(f);
    const restarted = differs && !newRound && !seq16Newer(f.stateSeq, this.round.seq);
    if (differs) {
      if (newRound) {
        this.round = { id: f.roundId };
        this._resetValue();
        this.presses = [];
      }
      if (restarted) {
        // The new bridge holds no values yet: report the own one again.
        this.round.startMs = undefined;
        if (this.val !== null) {
          this.confirmed = false;
          this.sendArmed = true;
          this.retryMs = 0;
          this.nextSendAt = now + this.world.rnd.int(T.retryMin, T.retryMax);
        }
      }
      const prevPhase = this.round.phase;
      Object.assign(this.round, {
        seq: f.stateSeq, type: f.roundType, phase: f.phase, keys: f.keys, hold: f.hold,
        min: f.min, max: f.max, redMax: f.redMax, greenMin: f.greenMin, winner: f.winner, correct: f.correct,
      });
      if (this.round.phase === 'open' && prevPhase !== 'open') {
        this.round.justOpened = true;
        if (this.round.hold === 'fest') this.round.fixedLage = this.lage; // no switching in this round
      }
      if (newRound) this.world.person.onRound(this, this.round, now);
    }
    if (f.roundType !== 'idle' && f.phase !== 'announced') {
      const cand = now - f.openOffsetMs;
      if (this.round.startMs === undefined || cand < this.round.startMs) this.round.startMs = cand;
    }
    if (this.round.justOpened && this.round.startMs !== undefined) {
      this.round.justOpened = false;
      this.world.person.onOpen(this, this.round);
    }
    if (this.round.type === 'move') {
      // The echo carries the last press counter the gateway passed on.
      const echo = this.number ? f.echo[this.number - f.echoFirst] : undefined;
      if (echo !== undefined && echo !== null && this.keyLog.length) {
        this.keyLog = this.keyLog.filter((p) => seq8Newer(p.s, echo));
        if (!this.keyLog.length) this.keyRetryMs = 0;
      }
      return;
    }
    if (!this.number || this.val === null) return;
    const echo = f.echo[this.number - f.echoFirst];
    if (echo === undefined) return;
    if (echo === this.val) {
      this.confirmed = true;
      this.sendArmed = false;
    } else if (this.confirmed) {
      this.confirmed = false;
      this.sendArmed = true;
      this.retryMs = 0;
      this.nextSendAt = now + this.world.rnd.int(T.retryMin, T.retryMax);
    }
  }

  _sameState(f) {
    const r = this.round;
    const keys = (k) => JSON.stringify(Object.entries(k || {}).sort());
    return f.stateSeq === r.seq && f.roundType === r.type && f.phase === r.phase && keys(f.keys) === keys(r.keys) &&
      f.hold === r.hold && f.min === r.min && f.max === r.max && f.redMax === r.redMax && f.greenMin === r.greenMin &&
      f.winner === r.winner && f.correct === r.correct;
  }

  // ── Orientation ──────────────────────────────────────────────────────────
  // The lage the buttons are read in: with hold "fest" the one at the open.
  inputLage() {
    const r = this.round;
    return r && r.hold === 'fest' && r.fixedLage ? r.fixedLage : this.lage;
  }

  // In a round that requires another lage the buzzer takes no input.
  locked() {
    const r = this.round;
    const need = this.state === 'member' && r && r.type !== 'idle' ? requiredLage(r.hold) : null;
    return !!need && !fitsLage(this.lage, need);
  }

  // The keyboard: the buzzer is in this lage at once.
  setLage(lage, now) {
    this.turn = null;
    this.physLage = lage;
    this.physAt = now;
    if (this.lage !== lage) this._takeLage(lage);
  }

  _takeLage(lage) {
    this.lage = lage;
    if (this.state === 'member') this._sendStatus(0); // a new lage goes out at once
  }

  _tickLage(now) {
    if (this.turn && now >= this.turn.at) {
      this.physLage = this.turn.to;
      this.physAt = now;
      this.turn = null;
    }
    if (this.physLage !== this.lage && now - this.physAt >= this.world.lt.lageSettleMs) this._takeLage(this.physLage);
  }

  // ── Keys ─────────────────────────────────────────────────────────────────
  // A physical button: the buzzer remaps it with the lage it reads it in.
  pressButton(button, ms, now) {
    const key = toLogical(this.inputLage(), button);
    if (key) this.press(key, ms, now);
  }

  // A logical input (up, right, down, left, triggerLeft, triggerRight,
  // buzzer, enter; "buzz" counts as buzzer), reported with this lage.
  press(key, ms, now, lage = this.inputLage()) {
    if (this.power.blocksGame(now)) return;
    this.power.wake();
    const name = key === 'buzzer' ? 'buzz' : key; // the round's keys use the protocol names
    if (this.state === 'normal') {
      if (name === 'buzz' && this.invite && now - this.invite.at < T.inviteVisible) this._joinFromInvite(now);
      return;
    }
    if (this.state !== 'member' || this.locked()) return;
    const r = this.round;
    const free = r && r.type !== 'idle' && r.phase === 'open' && r.keys && name in r.keys &&
      r.startMs !== undefined && ms - r.startMs >= 0;
    if (!free) return;
    this.valLage = lage;
    if (r.type === 'race') {
      if (name === 'buzz' && this.val === null) this._setValue(1, ms, now, 0);
    } else if (r.type === 'choice') {
      const a = CHOICE_KEYS.indexOf(key) + 1;
      if (a > 0 && a !== this.val) this._setValue(a, ms, now, 0);
    } else if (r.type === 'move') {
      this._pressKey(key, lage, now);
    } else if (r.type === 'scale') {
      const steps = r.max - r.min + 1;
      let v;
      if (this.val === null) v = key === 'up' ? r.min + Math.floor(steps / 2) : r.min + Math.floor((steps - 1) / 2);
      else if (key === 'up') v = Math.min(this.val + 1, r.max);
      else v = Math.max(this.val - 1, r.min);
      if (v !== this.val) this._setValue(v, ms, now, T.scaleSendDelay);
    }
  }

  _setValue(v, ms, now, delay) {
    this.val = v;
    this.vseq = (this.vseq + 1) & 0xff;
    this.inputMs = ms;
    this.confirmed = false;
    this.sendArmed = true;
    this.retryMs = 0;
    this.nextSendAt = delay === 0 ? now : ms + delay;
    this._serviceSend(now);
  }

  _resetValue() {
    this.val = null;
    this.vseq = 0;
    this.inputMs = 0;
    this.confirmed = false;
    this.sendArmed = false;
    this.retryMs = 0;
    this.keySeq = 0;
    this.keyLog = [];
    this.keyRetryMs = 0;
    this.nextKeysAt = 0;
  }

  // Move rounds: every press counts. The buzzer numbers its presses (8 bit,
  // per round) and sends all unconfirmed ones until the echo shows the newest.
  _pressKey(key, lage, now) {
    this.keySeq = (this.keySeq + 1) & 0xff;
    this.keyLog.push({ s: this.keySeq, k: key, l: lage });
    if (this.keyLog.length > MOVE_LOG) this.keyLog.shift();
    this.keyRetryMs = 0;
    this.nextKeysAt = now;
    this._serviceKeys(now);
  }

  _serviceKeys(now) {
    if (!this.keyLog.length || !this.round || now < this.nextKeysAt) return;
    this._send({ type: 'keys', origin: this.number, session: this.session, roundId: this.round.id, presses: this.keyLog.slice() });
    this.keyRetryMs = this.keyRetryMs === 0 ? this.world.rnd.int(T.retryMin, T.retryMax) : Math.min(this.keyRetryMs * 2, T.retryCap);
    this.nextKeysAt = now + this.keyRetryMs;
  }

  // ── Time ─────────────────────────────────────────────────────────────────
  tick(now) {
    this.power.tick(now);
    this._tickLage(now);
    if (this.presses.length && this.presses[0].at <= now && this.locked()) {
      // The person waits until the buzzer takes the lage, then goes on.
      const later = now + LOCKED_RETRY_MS - this.presses[0].at;
      for (const p of this.presses) p.at += later;
    }
    while (this.presses.length && this.presses[0].at <= now) {
      const p = this.presses.shift();
      if (p.move) this.world.person.onMoveTurn(this, p.at, now);
      else this.pressButton(toPhysical(this.physLage, p.key) ?? p.key, p.at, now);
    }
    switch (this.state) {
      case 'normal':
        return;
      case 'booting':
        if (now >= this.bootDone) {
          this.state = 'rejoinListen';
          this.deadline = now + T.rejoinListen;
          this.channel = this.sessionChannel;
        }
        return;
      case 'rejoinListen':
        if (now >= this.deadline) this._leave();
        return;
      case 'joining':
        if (now - this.joinStart >= T.joinTimeout) {
          this._leave();
          return;
        }
        if (now >= this.nextJoinAt) {
          this._sendStatus(this.joinFlags);
          this.nextJoinAt = now + this.world.rnd.int(T.joinRepeatMin, T.joinRepeatMax);
        }
        return;
      default:
        break;
    }
    if (this.pendingChannel && now >= this.switchAt) {
      this._follow(this.pendingChannel);
      this.pendingChannel = 0;
    }
    if (!this.searching && now - this.lastGateway >= T.noGatewaySearch) {
      this.searching = true;
      this.searchNextAt = now;
    }
    if (this.searching && now >= this.searchNextAt) {
      this.channel = (this.channel % 13) + 1;
      this.searchNextAt = now + T.searchDwell;
    }
    this._serviceSend(now);
    this._serviceKeys(now);
    if (now >= this.nextStatusAt) {
      this._sendStatus(0);
      this.nextStatusAt = now + this._statusInterval();
    }
  }

  _serviceSend(now) {
    if (!this.sendArmed || this.confirmed || now < this.nextSendAt) return;
    this._send({
      type: 'report', origin: this.number, session: this.session, roundId: this.round.id,
      // Against the current round start, which a bridge restart moves.
      valueSeq: this.vseq, value: this.val,
      pressOffsetMs: this.round.startMs !== undefined ? Math.round(this.inputMs - this.round.startMs) : 0,
      battery: this.battery, lage: this.valLage,
    });
    this.retryMs = this.retryMs === 0 ? this.world.rnd.int(T.retryMin, T.retryMax) : Math.min(this.retryMs * 2, T.retryCap);
    this.nextSendAt = now + this.retryMs;
  }

  // ── Session ──────────────────────────────────────────────────────────────
  _joinFromInvite(now) {
    this.session = this.invite.session;
    this.sessionChannel = this.invite.channel;
    this.channel = this.invite.channel;
    this.lastGateway = now;
    this._startJoin(true, false, now);
  }

  _startJoin(pressed, rejoin, now) {
    this.state = 'joining';
    this.joinStart = now;
    this.joinFlags = { join: true, pressed, rejoin };
    this._sendStatus(this.joinFlags);
    this.nextJoinAt = now + this.world.rnd.int(T.joinRepeatMin, T.joinRepeatMax);
  }

  // Leaving always ends in a restart into normal operation.
  _leave() {
    this.power.dirty = true; // fleet status again right away
    this.state = 'normal';
    this.session = 0;
    this.channel = this.homeChannel;
    this.invite = null;
    this.joinPlanned = false;
    this.round = null;
    this.presses = [];
    this._resetValue();
  }

  // Power cycle in the middle of a session: the stored session entry makes
  // the device listen for its bridge and rejoin without a press.
  reboot(now) {
    if (this.state !== 'member') return false;
    this.state = 'booting';
    this.bootDone = now + this.world.rnd.int(2000, 4000);
    this.round = null;
    this.presses = [];
    this.msgId = 0;
    this._resetValue();
    return true;
  }

  _statusInterval() {
    return this.world.rnd.int(T.statusPeriod - T.statusJitter, T.statusPeriod + T.statusJitter);
  }

  _sendStatus(flags) {
    const f = flags || {};
    this._send({
      type: 'status', origin: this.number || NO_NUMBER, fleet: 0, session: this.session, mac: this.mac,
      fw: this.power.fw, battery: this.battery, charging: this.power.chg === 'charging',
      join: !!f.join, pressed: !!f.pressed, rejoin: !!f.rejoin, lage: this.lage,
      lastRoundId: this.round ? this.round.id : 0, lastStateSeq: this.round ? this.round.seq : 0,
    });
  }

  _send(frame) {
    frame.msgId = this.msgId++;
    this.world.radio.fromDevice(this, frame);
  }
}

// What the people holding the buzzers do. Every value comes from the seeded
// random source, per round from a shared mood so results look plausible.
export class Person {
  constructor(world) {
    this.world = world;
    this.moods = new Map();
  }

  wantsToJoin() {
    return true;
  }

  _mood(round) {
    let m = this.moods.get(round.id);
    if (!m) {
      const rnd = this.world.rnd;
      const weights = [0, 1, 2, 3].map(() => 0.3 + rnd() * 2);
      weights[rnd.int(0, 3)] += 2.5; // one answer is popular
      m = { centre: rnd.int(2, 8), spread: 1 + rnd() * 1.5, weights };
      this.moods.set(round.id, m);
    }
    return m;
  }

  // A new round: one that requires a lage makes most people turn their
  // buzzer into it, after a moment; a few take long, some never do.
  onRound(dev, round, now) {
    dev.turn = null;
    const need = round.type !== 'idle' ? requiredLage(round.hold) : null;
    if (!need || fitsLage(dev.physLage, need) || dev.manual) return;
    const lt = this.world.lt;
    const rnd = this.world.lrnd;
    const x = rnd();
    if (x < lt.turnNever) return;
    const slow = x < lt.turnNever + lt.turnSlow;
    dev.turn = { at: now + (slow ? rnd.int(lt.turnSlowMin, lt.turnSlowMax) : rnd.int(lt.turnMin, lt.turnMax)), to: need };
  }

  onOpen(dev, round) {
    const rnd = this.world.rnd;
    const start = round.startMs;
    const mood = this._mood(round);
    const add = (at, key) => {
      dev.presses.push({ at, key });
    };
    if (round.type === 'race') {
      if (rnd.chance(0.06)) return; // asleep
      // Reaction time, a few close calls among the fast ones.
      const base = rnd.chance(0.15) ? rnd.int(180, 260) : rnd.int(230, 1200);
      add(start + base, 'buzz');
    } else if (round.type === 'scale') {
      if (rnd.chance(0.08)) return;
      const lo = round.min ?? 0;
      const hi = round.max ?? 9;
      const target = Math.max(lo, Math.min(hi, Math.round(mood.centre + rnd.normal() * mood.spread)));
      const steps = hi - lo + 1;
      const upFirst = lo + Math.floor(steps / 2);
      const downFirst = lo + Math.floor((steps - 1) / 2);
      let at = Math.max(start, this.world.now()) + rnd.int(400, 4000);
      let v;
      if (target >= upFirst) {
        add(at, 'up');
        v = upFirst;
      } else {
        add(at, 'down');
        v = downFirst;
      }
      while (v !== target) {
        at += rnd.int(150, 450);
        if (v < target) {
          add(at, 'up');
          v++;
        } else {
          add(at, 'down');
          v--;
        }
      }
      if (rnd.chance(0.12)) {
        // Second thoughts: one or two steps back.
        at += rnd.int(1500, 4000);
        const back = v > lo && (v === hi || rnd.chance(0.5)) ? 'down' : 'up';
        add(at, back);
      }
    } else if (round.type === 'choice') {
      if (rnd.chance(0.06)) return;
      const keys = CHOICE_KEYS.filter((k) => round.keys && k in round.keys);
      const pick = () => {
        const w = mood.weights.slice(0, keys.length);
        let x = rnd() * w.reduce((a, b) => a + b, 0);
        for (let i = 0; i < w.length; i++) {
          x -= w[i];
          if (x <= 0) return keys[i];
        }
        return keys[keys.length - 1];
      };
      let at = Math.max(start, this.world.now()) + rnd.int(800, 6000);
      add(at, pick());
      if (rnd.chance(0.1)) {
        at += rnd.int(1000, 3000);
        add(at, pick());
      }
    } else if (round.type === 'move') {
      const mt = this.world.mt;
      if (dev.pace === undefined) dev.pace = rnd.int(mt.movePaceMin, mt.movePaceMax);
      dev.presses.push({ at: Math.max(start, this.world.now()) + rnd.int(mt.moveFirstMin, mt.moveFirstMax), move: true });
    }
    dev.presses.sort((a, b) => a.at - b.at);
  }

  // One turn in a move round: a step in a random direction with a clear pull
  // towards the goal, pressed on the button that points there the way the
  // buzzer is held. The person sees the board: no board, no play; in the
  // goal, done. While the keyboard controls the buzzer the person waits.
  onMoveTurn(dev, at, now) {
    const r = dev.round;
    const board = this.world.board();
    if (!r || r.type !== 'move' || r.phase !== 'open' || !board || board.finished.includes(dev.number)) return;
    const rnd = this.world.rnd;
    if (!dev.manual) {
      const x = rnd();
      const dir = x < 0.6 ? 'right' : x < 0.75 ? 'up' : x < 0.9 ? 'down' : 'left';
      dev.pressButton(toPhysical(dev.physLage, dir), at, now);
    }
    const next = at + Math.round(dev.pace * (0.6 + rnd() * 0.8));
    dev.presses.push({ at: Math.max(next, now + 1), move: true });
    dev.presses.sort((a, b) => a.at - b.at);
  }
}
