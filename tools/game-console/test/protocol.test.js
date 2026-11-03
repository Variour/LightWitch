import { test } from 'node:test';
import assert from 'node:assert/strict';
import { LineSplitter, parseLine, serialize, cmd, keysFor, normalizeMac, seq8Newer, LINE_MAX } from '../src/protocol.js';

test('line splitter joins chunks and accepts \\n and \\r\\n', () => {
  const s = new LineSplitter();
  assert.deepEqual(s.push('{"ns":"link"'), []);
  assert.deepEqual(s.push(',"ev":"pong"}\r\n{"a":'), ['{"ns":"link","ev":"pong"}']);
  assert.deepEqual(s.push(Buffer.from('1}\n\n')), ['{"a":1}', '']);
});

test('line splitter drops an over-long line up to its end', () => {
  const s = new LineSplitter(10);
  assert.deepEqual(s.push('x'.repeat(25)), []);
  assert.deepEqual(s.push('yy\nok\n'), ['ok']);
});

test('parseLine ignores boot messages, log lines and JSON without ns', () => {
  assert.equal(parseLine('ESP-ROM:esp32c5-20240829'), null);
  assert.equal(parseLine('[game] session 12 entry found'), null);
  assert.equal(parseLine('{"ev":"hello"}'), null);
  assert.equal(parseLine('[1,2]'), null);
  assert.equal(parseLine('{broken'), null);
  assert.deepEqual(parseLine(' {"ns":"game","ev":"value","n":17} '), { ns: 'game', ev: 'value', n: 17 });
});

test('serialize writes one line and refuses more than 512 bytes', () => {
  assert.equal(serialize({ ns: 'link', cmd: 'ping', id: 8 }), '{"ns":"link","cmd":"ping","id":8}\n');
  assert.throws(() => serialize({ ns: 'game', cmd: 'x', pad: 'a'.repeat(LINE_MAX) }));
});

test('round command carries the whole state per type', () => {
  const scale = { roundId: 23417, type: 'scale', phase: 'announced', keys: keysFor('scale'), min: 0, max: 9, redMax: 3, greenMin: 7 };
  assert.deepEqual(cmd.round(scale), {
    ns: 'game', cmd: 'round', round: 23417, type: 'scale', phase: 'announced', keys: { up: 'green', down: 'red' }, min: 0, max: 9, redMax: 3, greenMin: 7,
  });
  const race = { roundId: 5, type: 'race', phase: 'open', keys: keysFor('race'), winner: 0 };
  assert.deepEqual(cmd.round(race, { openInMs: 1000 }), { ns: 'game', cmd: 'round', round: 5, type: 'race', phase: 'open', keys: { buzz: 'white' }, openInMs: 1000 });
  assert.equal(cmd.round({ ...race, winner: 17 }).winner, 17);
  assert.equal('openInMs' in cmd.round({ ...race, phase: 'closed' }, { openInMs: 1000 }), false);
  const choice = { roundId: 6, type: 'choice', phase: 'closed', keys: keysFor('choice', 3), correct: 2, revealed: false };
  assert.equal('correct' in cmd.round(choice), false);
  assert.equal(cmd.round({ ...choice, revealed: true }).correct, 2);
  assert.deepEqual(cmd.round({ roundId: 7, type: 'idle', phase: null }), { ns: 'game', cmd: 'round', round: 7, type: 'idle' });
});

test('choice keys go up, right, down, left with their colours', () => {
  assert.deepEqual(keysFor('choice', 2), { up: 'yellow', right: 'blue' });
  assert.deepEqual(keysFor('choice', 4), { up: 'yellow', right: 'blue', down: 'magenta', left: 'cyan' });
  assert.equal(keysFor('idle'), null);
});

test('number command reads, sets and clears', () => {
  assert.deepEqual(cmd.number(), { ns: 'game', cmd: 'number' });
  assert.deepEqual(cmd.number(17), { ns: 'game', cmd: 'number', n: 17 });
  assert.deepEqual(cmd.number(null), { ns: 'game', cmd: 'number', n: null });
});

test('MAC addresses are normalised to lowercase', () => {
  assert.equal(normalizeMac('02:00:00:00:00:2A'), '02:00:00:00:00:2a');
  assert.equal(normalizeMac('02-00-00-00-00-2a'), null);
  assert.equal(normalizeMac(17), null);
});

test('vseq comparison wraps at 256', () => {
  assert.equal(seq8Newer(4, 3), true);
  assert.equal(seq8Newer(0, 255), true);
  assert.equal(seq8Newer(3, 3), false);
  assert.equal(seq8Newer(3, 4), false);
  assert.equal(seq8Newer(130, 2), false);
});
