import { EventEmitter } from 'node:events';

export const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

export async function waitFor(fn, { timeoutMs = 5000, stepMs = 20, what = 'condition' } = {}) {
  const until = Date.now() + timeoutMs;
  for (;;) {
    const v = fn();
    if (v) return v;
    if (Date.now() > until) throw new Error(`timed out waiting for ${what}`);
    await sleep(stepMs);
  }
}

// Stand-in for GatewayLink: records every command and answers through
// `reply(cmd)` (return an event object, an { error } object, or null to stay
// silent). Tests push gateway events with `emitEvent`.
export class FakeLink extends EventEmitter {
  constructor(reply = defaultReply) {
    super();
    this.connected = true;
    this.state = 'connected';
    this.port = { path: 'fake' };
    this.hello = { fw: '1a2b3c4d', mac: '02:00:00:00:00:01' };
    this.counters = null;
    this.transportStatus = '';
    this.sent = [];
    this.reply = reply;
  }

  request(command) {
    this.sent.push(command);
    const r = this.reply(command);
    if (!r) return new Promise(() => {});
    if (r.error) return Promise.reject(Object.assign(new Error(r.error), { code: r.error }));
    return Promise.resolve(r);
  }

  emitEvent(ev) {
    this.emit('event', ev);
  }

  cmds(name) {
    return this.sent.filter((c) => c.cmd === name);
  }
}

export function defaultReply(c) {
  if (c.cmd === 'session') return { ns: 'game', ev: 'session', session: c.session, channel: c.channel, fleet: c.fleet, login: c.login, active: true };
  if (c.cmd === 'admit') return { ns: 'game', ev: 'admitted', mac: c.mac, n: c.n };
  if (c.cmd === 'round') return { ns: 'game', ev: 'state', round: c.round, seq: 1, type: c.type, phase: c.phase ?? null, changed: true, sinceOpenMs: null, closedAtMs: null };
  if (c.cmd === 'release') return { ns: 'game', ev: 'released', mac: c.mac };
  if (c.cmd === 'end') return { ns: 'game', ev: 'session', session: 1, channel: 6, fleet: 0, login: false, active: false };
  if (c.cmd === 'number') return { ns: 'game', ev: 'number', n: c.n ?? null };
  return { ns: 'link', ev: 'pong' };
}

export const join = (mac, n, extra = {}) => ({
  ns: 'game', ev: 'join', mac, n, fw: '1a2b3c4d', proto: 1, fleet: 0, bat: 80, chg: false, rssi: -60, pressed: true, rejoin: false, ...extra,
});

export const value = (n, round, v, extra = {}) => ({
  ns: 'game', ev: 'value', n, round, v, vseq: 1, t: 1000, arr: 1, late: false, rssi: -60, bat: 80, ...extra,
});

export const device = (n, mac, extra = {}) => ({
  ns: 'game', ev: 'device', n, mac, fw: '1a2b3c4d', proto: 1, bat: 80, chg: false, rssi: -60, round: 0, seq: 1, ...extra,
});

export const flush = () => new Promise((resolve) => setImmediate(resolve));

export const ROUNDS = {
  scale: { format: 'buzzer-round/0', id: 'runde-4', title: 'Runde 4', question: 'Wie viele Punkte Zustimmung?', type: 'scale', min: 0, max: 9, redMax: 3, greenMin: 7 },
  race: { format: 'buzzer-round/0', id: 'schnellrunde', title: 'Schnellrunde', question: 'Wie heißt die Hauptstadt von Australien?', type: 'race' },
  choice: { format: 'buzzer-round/0', id: 'quiz-3', title: 'Quizfrage 3', question: 'Wie funken die Buzzer untereinander?', type: 'choice', answers: ['ESP-NOW', 'Bluetooth', 'WLAN mit Router', 'Zigbee'], correct: 1 },
  idle: { format: 'buzzer-round/0', id: 'pause', title: 'Pause', type: 'idle' },
};
