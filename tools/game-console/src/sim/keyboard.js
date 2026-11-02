// Keyboard test aid for `--simulate`: the arrow keys move one simulated
// buzzer on the board of a move round, Tab picks the next participant. An
// arrow names the direction on the board: the buzzer sends it as a logical
// input with a lage, the one the round requires or else "quer".

import { requiredLage } from '../lage.js';

const ARROWS = { A: 'up', B: 'down', C: 'right', D: 'left' };

// Splits terminal input into keys: 'up', 'down', 'right', 'left' (ESC [ A-D,
// also with modifiers, and ESC O A-D), 'tab' and 'quit' (Ctrl+C). Other input
// is skipped. An escape sequence cut off at the end stays in `rest` for the
// next chunk.
export function parseKeys(text) {
  const keys = [];
  let i = 0;
  while (i < text.length) {
    const ch = text[i];
    if (ch === '\x1b') {
      if (i + 1 >= text.length) break;
      const intro = text[i + 1];
      if (intro !== '[' && intro !== 'O') {
        i += 1;
        continue;
      }
      let j = i + 2;
      while (j < text.length && /[0-9;]/.test(text[j])) j++;
      if (j >= text.length) break;
      if (ARROWS[text[j]]) keys.push(ARROWS[text[j]]);
      i = j + 1;
      continue;
    }
    if (ch === '\t') keys.push('tab');
    else if (ch === '\x03') keys.push('quit');
    i += 1;
  }
  return { keys, rest: text.slice(i) };
}

// participants(): admitted numbers, ascending. hold(): the hold of the
// running move round, or null. press(n, dir, lage): the buzzer with number n
// sends this logical direction in this lage. control(n): the keyboard takes
// buzzer n, the others play on their own. say(text): one line in the
// terminal. quit(): Ctrl+C.
export class SimKeyboard {
  constructor({ participants, hold, press, control, say, quit }) {
    Object.assign(this, { participants, hold, press, control, say, quit });
    this.n = null;
    this.rest = '';
  }

  feed(chunk) {
    const { keys, rest } = parseKeys(this.rest + String(chunk));
    this.rest = rest;
    for (const k of keys) this._key(k);
  }

  _key(k) {
    if (k === 'quit') {
      this.quit();
      return;
    }
    const ns = this.participants();
    if (k === 'tab') {
      this._take(ns.find((n) => this.n === null || n > this.n) ?? ns[0]);
      return;
    }
    const hold = this.hold();
    if (!hold) return;
    if (this.n === null || !ns.includes(this.n)) this._take(ns[0]);
    if (this.n !== null && ns.includes(this.n)) this.press(this.n, k, requiredLage(hold) ?? 'quer');
  }

  _take(n) {
    if (n === undefined || n === this.n) return;
    this.n = n;
    this.control(n);
    this.say(`Tastatur steuert Nr. ${n}`);
  }
}

// The keyboard for a console started with `--simulate`.
export function simKeyboard(app, { say, quit }) {
  const gw = app.transport;
  const member = (n) => gw.devices.find((d) => d.state === 'member' && d.number === n);
  return new SimKeyboard({
    participants: () => app.game.snapshot().participants.filter((p) => p.admitted).map((p) => p.n),
    hold: () => {
      const r = app.game.round;
      return r && r.type === 'move' && !r.adopted ? r.hold : null;
    },
    press: (n, dir, lage) => {
      const d = member(n);
      if (!d) return;
      d.setLage(lage, gw.now()); // turned at once, so the buzzer is not locked
      d.press(dir, gw.now(), gw.now(), lage);
    },
    control: (n) => {
      for (const d of gw.devices) d.manual = d.number === n;
    },
    say,
    quit,
  });
}
