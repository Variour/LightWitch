// Simulated Buzzerboards for the fake gateway. The device logic follows
// src/game/Participant.h on the game-session branch (join, admit, echo,
// resend, status, rejoin after a restart); a small "person" on top presses
// keys like a participant would.

import { seq16Newer } from '../protocol.js';

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

const NO_NUMBER = 0xffff;
const CHOICE_KEYS = ['up', 'right', 'down', 'left'];

export class SimDevice {
  constructor(world, { index, mac, storedN, channel, battery, rssi }) {
    this.world = world;
    this.index = index;
    this.mac = mac;
    this.number = storedN || 0;
    this.homeChannel = channel; // channel in normal operation
    this.channel = channel;
    this.battery = battery;
    this.rssiBase = rssi;
    this.state = 'normal'; // normal | booting | rejoinListen | joining | member
    this.session = 0;
    this.sessionChannel = 0;
    this.msgId = 0;
    this.invite = null; // { at, session, channel }
    this.joinPlanned = false;
    this.presses = []; // scheduled key presses { at, key }
    this.round = null;
    this._resetValue();
    this.lastGateway = 0;
    this.searching = false;
  }

  get rssi() {
    return this.rssiBase + this.world.rnd.int(-3, 3);
  }

  // ── Radio in ─────────────────────────────────────────────────────────────
  onFrame(f, now) {
    if (f.origin !== 0 || this.state === 'booting') return;
    if (this.state === 'normal') {
      if (f.type === 'invite' && (f.fleet === 0)) {
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
    const newRound = !this.round || f.roundId !== this.round.id;
    if (newRound || seq16Newer(f.stateSeq, this.round.seq)) {
      if (newRound) {
        this.round = { id: f.roundId };
        this._resetValue();
        this.presses = [];
      }
      const prevPhase = this.round.phase;
      Object.assign(this.round, {
        seq: f.stateSeq, type: f.roundType, phase: f.phase, keys: f.keys,
        min: f.min, max: f.max, winner: f.winner, correct: f.correct,
      });
      if (this.round.phase === 'open' && prevPhase !== 'open') this.round.justOpened = true;
    }
    if (f.roundId !== this.round.id) return;
    if (f.roundType !== 'idle' && f.phase !== 'announced') {
      const cand = now - f.openOffsetMs;
      if (this.round.startMs === undefined || cand < this.round.startMs) this.round.startMs = cand;
    }
    if (this.round.justOpened && this.round.startMs !== undefined) {
      this.round.justOpened = false;
      this.world.person.onOpen(this, this.round);
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

  // ── Keys ─────────────────────────────────────────────────────────────────
  press(key, ms, now) {
    if (this.state === 'normal') {
      if (key === 'buzz' && this.invite && now - this.invite.at < T.inviteVisible) this._joinFromInvite(now);
      return;
    }
    if (this.state !== 'member') return;
    const r = this.round;
    const free = r && r.type !== 'idle' && r.phase === 'open' && r.keys && key in r.keys &&
      r.startMs !== undefined && ms - r.startMs >= 0;
    if (!free) return;
    if (r.type === 'race') {
      if (key === 'buzz' && this.val === null) this._setValue(1, ms, now, 0);
    } else if (r.type === 'choice') {
      const a = CHOICE_KEYS.indexOf(key) + 1;
      if (a > 0 && a !== this.val) this._setValue(a, ms, now, 0);
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
    this.t = Math.round(ms - this.round.startMs);
    this.confirmed = false;
    this.sendArmed = true;
    this.retryMs = 0;
    this.nextSendAt = delay === 0 ? now : ms + delay;
    this._serviceSend(now);
  }

  _resetValue() {
    this.val = null;
    this.vseq = 0;
    this.t = 0;
    this.confirmed = false;
    this.sendArmed = false;
    this.retryMs = 0;
  }

  // ── Time ─────────────────────────────────────────────────────────────────
  tick(now) {
    while (this.presses.length && this.presses[0].at <= now) {
      const p = this.presses.shift();
      this.press(p.key, p.at, now);
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
    if (now >= this.nextStatusAt) {
      this._sendStatus(0);
      this.nextStatusAt = now + this._statusInterval();
    }
  }

  _serviceSend(now) {
    if (!this.sendArmed || this.confirmed || now < this.nextSendAt) return;
    this._send({
      type: 'report', origin: this.number, session: this.session, roundId: this.round.id,
      valueSeq: this.vseq, value: this.val, pressOffsetMs: this.t, battery: this.battery,
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
      fw: this.world.fw, battery: this.battery, charging: false,
      join: !!f.join, pressed: !!f.pressed, rejoin: !!f.rejoin,
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
    }
    dev.presses.sort((a, b) => a.at - b.at);
  }
}
