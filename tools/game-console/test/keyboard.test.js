import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseKeys, SimKeyboard } from '../src/sim/keyboard.js';

function keyboard({ participants = [3, 7, 12], hold = 'quer' } = {}) {
  const log = { presses: [], control: [], said: [], quit: 0 };
  const env = { participants, hold };
  const kb = new SimKeyboard({
    participants: () => env.participants,
    hold: () => env.hold,
    press: (n, key) => log.presses.push([n, key]),
    control: (n) => log.control.push(n),
    say: (text) => log.said.push(text),
    quit: () => {
      log.quit++;
    },
  });
  return { kb, log, env };
}

test('terminal input: arrows in both cursor modes and with modifiers, Tab, Ctrl+C', () => {
  assert.deepEqual(parseKeys('\x1b[A\x1b[B\x1b[C\x1b[D'), { keys: ['up', 'down', 'right', 'left'], rest: '' });
  assert.deepEqual(parseKeys('\x1bOA\x1bOD'), { keys: ['up', 'left'], rest: '' });
  assert.deepEqual(parseKeys('\x1b[1;2C\t\x03'), { keys: ['right', 'tab', 'quit'], rest: '' });
  assert.deepEqual(parseKeys('x\r\x1b[Z\x1b[5~a'), { keys: [], rest: '' }); // other keys are skipped
  assert.deepEqual(parseKeys('\x1b[A\x1b['), { keys: ['up'], rest: '\x1b[' });
  assert.deepEqual(parseKeys('\x1b'), { keys: [], rest: '\x1b' });
  assert.deepEqual(parseKeys('\x1b\x1b[B'), { keys: ['down'], rest: '' }); // a lone Esc before an arrow
});

test('the first arrow takes the lowest number and presses the key that points that way', () => {
  const { kb, log } = keyboard();
  kb.feed('\x1b[A'); // board up: quer buzzer key left
  assert.deepEqual(log.said, ['Tastatur steuert Nr. 3']);
  assert.deepEqual(log.control, [3]);
  kb.feed('\x1b[C\x1b[B\x1b[D');
  assert.deepEqual(log.presses, [[3, 'left'], [3, 'up'], [3, 'right'], [3, 'down']]);
});

test('hochkant: the arrows are the buzzer keys', () => {
  const { kb, log } = keyboard({ hold: 'hochkant' });
  kb.feed('\x1b[A\x1b[C\x1b[B\x1b[D');
  assert.deepEqual(log.presses.map(([, k]) => k), ['up', 'right', 'down', 'left']);
});

test('Tab walks through the admitted numbers and wraps', () => {
  const { kb, log, env } = keyboard();
  kb.feed('\t\t\t\t');
  assert.deepEqual(log.control, [3, 7, 12, 3]);
  assert.deepEqual(log.said.at(-1), 'Tastatur steuert Nr. 3');
  env.participants = [3];
  kb.feed('\t');
  assert.equal(log.said.length, 4); // still 3, nothing new to say
  env.participants = [5, 9];
  kb.feed('\x1b[C'); // 3 is gone: the arrow takes the lowest number
  assert.deepEqual(log.presses, [[5, 'up']]);
});

test('arrows do nothing without a move round; keys split across reads still count', () => {
  const { kb, log, env } = keyboard({ hold: null });
  kb.feed('\x1b[A');
  assert.deepEqual([log.presses, log.said], [[], []]);
  env.hold = 'quer';
  kb.feed('\x1b');
  kb.feed('[');
  kb.feed('C');
  assert.deepEqual(log.presses, [[3, 'up']]);
});

test('Ctrl+C quits', () => {
  const { kb, log } = keyboard();
  kb.feed('\x03');
  assert.equal(log.quit, 1);
});

test('no participant: nothing to control', () => {
  const { kb, log } = keyboard({ participants: [] });
  kb.feed('\t\x1b[A');
  assert.deepEqual([log.presses, log.control, log.said], [[], [], []]);
});
