// Gateway protocol v0: NDJSON lines between the console (laptop) and the
// gateway buzzer over USB. One JSON object per line, namespaces `link` and
// `game`. The firmware (src/game/Link.h) is the reference where the docs and
// the firmware differ.

export const LINE_MAX = 512; // bytes per line, both directions
export const LINK_VERSION = 0;

export const ROUND_TYPES = ['idle', 'race', 'scale', 'choice'];
export const PHASES = ['announced', 'open', 'closed'];
export const KEY_NAMES = ['buzz', 'up', 'down', 'left', 'right', 'enter'];
export const COLOR_NAMES = ['white', 'red', 'green', 'yellow', 'blue', 'magenta', 'cyan', 'orange'];

// Choice answers 1-4 sit on these keys, in this order (gateway protocol v0,
// "Erlaubte Tasten"; round format proposal 9 for the colours).
export const CHOICE_KEYS = ['up', 'right', 'down', 'left'];
export const CHOICE_COLORS = ['yellow', 'blue', 'magenta', 'cyan'];

// Keys a round of this type frees on the devices, with their colours.
export function keysFor(type, answerCount = 0) {
  if (type === 'race') return { buzz: 'white' };
  if (type === 'scale') return { up: 'green', down: 'red' };
  if (type === 'choice') {
    const keys = {};
    for (let i = 0; i < answerCount; i++) keys[CHOICE_KEYS[i]] = CHOICE_COLORS[i];
    return keys;
  }
  return null;
}

// Splits a byte or text stream into lines. Accepts "\n" and "\r\n". Lines
// longer than maxLen are dropped whole, up to their line end.
export class LineSplitter {
  constructor(maxLen = 64 * 1024) {
    this.maxLen = maxLen;
    this.buf = '';
    this.discard = false;
  }

  push(chunk) {
    const text = typeof chunk === 'string' ? chunk : chunk.toString('utf8');
    const lines = [];
    let start = 0;
    for (;;) {
      const nl = text.indexOf('\n', start);
      if (nl < 0) break;
      const part = this.buf + text.slice(start, nl);
      this.buf = '';
      start = nl + 1;
      if (this.discard) {
        this.discard = false;
        continue;
      }
      lines.push(part.endsWith('\r') ? part.slice(0, -1) : part);
    }
    if (!this.discard) {
      this.buf += text.slice(start);
      if (this.buf.length > this.maxLen) {
        this.buf = '';
        this.discard = true;
      }
    }
    return lines;
  }
}

// One protocol line from the gateway: a JSON object with a string `ns`.
// Anything else (boot messages, log lines, garbage) is ignored: null.
export function parseLine(line) {
  const s = line.trim();
  if (!s.startsWith('{')) return null;
  let obj;
  try {
    obj = JSON.parse(s);
  } catch {
    return null;
  }
  if (!obj || typeof obj !== 'object' || Array.isArray(obj)) return null;
  if (typeof obj.ns !== 'string') return null;
  return obj;
}

export function serialize(obj) {
  const line = JSON.stringify(obj) + '\n';
  if (Buffer.byteLength(line, 'utf8') > LINE_MAX) {
    throw new Error(`line longer than ${LINE_MAX} bytes: ${line.slice(0, 60)}...`);
  }
  return line;
}

// Builders for every command the console sends. `id` is filled in by the link.
export const cmd = {
  hello: () => ({ ns: 'link', cmd: 'hello', link: LINK_VERSION }),
  ping: () => ({ ns: 'link', cmd: 'ping' }),
  bye: () => ({ ns: 'link', cmd: 'bye' }),
  session: ({ session, channel, fleet, login }) => ({ ns: 'game', cmd: 'session', session, channel, fleet, login }),
  end: () => ({ ns: 'game', cmd: 'end' }),
  admit: (mac, n) => ({ ns: 'game', cmd: 'admit', mac, n }),
  release: (mac) => ({ ns: 'game', cmd: 'release', mac }),
  // Firmware addition: stores the number of the device on the USB cable
  // (null clears it, undefined only reads it).
  number: (n) => (n === undefined ? { ns: 'game', cmd: 'number' } : { ns: 'game', cmd: 'number', n }),
  // The whole round state. `openInMs` goes only with the switch to open.
  round: (r, { openInMs } = {}) => {
    const c = { ns: 'game', cmd: 'round', round: r.roundId, type: r.type };
    if (r.type === 'idle') return c;
    c.phase = r.phase;
    c.keys = r.keys;
    if (r.type === 'scale') {
      c.min = r.min;
      c.max = r.max;
      c.redMax = r.redMax;
      c.greenMin = r.greenMin;
    }
    if (openInMs !== undefined && r.phase === 'open') c.openInMs = openInMs;
    if (r.type === 'race' && r.winner) c.winner = r.winner;
    if (r.type === 'choice' && r.phase === 'closed' && r.revealed && r.correct) c.correct = r.correct;
    return c;
  },
};

export const MAC_RE = /^[0-9a-f]{2}(:[0-9a-f]{2}){5}$/;

export function normalizeMac(s) {
  if (typeof s !== 'string') return null;
  const m = s.trim().toLowerCase();
  return MAC_RE.test(m) ? m : null;
}

// Wrap-aware "a is newer than b" for the 8-bit valueSeq: (a - b) mod 256 in 1..127.
export function seq8Newer(a, b) {
  const d = (a - b) & 0xff;
  return d >= 1 && d <= 127;
}

// Wrap-aware "a is newer than b" for the 16-bit stateSeq.
export function seq16Newer(a, b) {
  const d = (a - b) & 0xffff;
  return d >= 1 && d <= 0x7fff;
}
