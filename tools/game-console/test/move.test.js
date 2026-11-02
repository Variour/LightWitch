import { test } from 'node:test';
import assert from 'node:assert/strict';
import { GameConsole } from '../src/game.js';
import { boardDirection, startCell } from '../src/move.js';
import { validateInput } from '../src/validate.js';
import { FakeLink, join, flush, ROUNDS } from './helpers.js';

const MAC = (i) => `02:00:00:00:${(i >> 8).toString(16).padStart(2, '0')}:${(i & 0xff).toString(16).padStart(2, '0')}`;

// A session with these numbers admitted and a move round open.
async function board(numbers, round = {}, { open = true } = {}) {
  const link = new FakeLink();
  const game = new GameConsole({ link, random: () => 0.5 });
  await game.act('session-start');
  for (const n of numbers) link.emitEvent(join(MAC(n), n));
  await flush();
  game.load(validateInput({ ...ROUNDS.move, ...round }));
  await game.act('next'); // announce, closes login
  if (open) await game.act('next');
  const seq = new Map();
  const keyEv = (n, k, kseq) => ({ ns: 'game', ev: 'key', n, round: game.round.roundId, key: k, kseq });
  const press = (n, ...keys) => {
    for (const k of keys) {
      const s = ((seq.get(n) ?? 0) + 1) & 0xff;
      seq.set(n, s);
      link.emitEvent(keyEv(n, k, s));
    }
  };
  const view = () => game.snapshot().round;
  const cell = (n) => {
    const p = view().results.players.find((x) => x.n === n);
    return p ? [p.col, p.row] : undefined;
  };
  return { link, game, press, keyEv, seq, view, cell };
}

test('physical keys: quer turns them 90° clockwise, every other hold takes them as printed', () => {
  assert.deepEqual(['left', 'up', 'right', 'down'].map((k) => boardDirection('quer', k, null)), ['up', 'right', 'down', 'left']);
  for (const hold of ['frei', 'hochkant', 'liegend', 'fest']) {
    assert.deepEqual(['up', 'right', 'down', 'left'].map((k) => boardDirection(hold, k, null)), ['up', 'right', 'down', 'left'], hold);
  }
  assert.equal(boardDirection('quer', 'buzz', null), null);
});

test('logical keys (a line with a lage) are never turned', () => {
  for (const hold of ['frei', 'hochkant', 'quer', 'liegend', 'fest']) {
    assert.deepEqual(['up', 'right', 'down', 'left'].map((k) => boardDirection(hold, k, 'quer')), ['up', 'right', 'down', 'left'], hold);
  }
  assert.equal(boardDirection('quer', 'buzzer', 'quer'), null);
  assert.equal(boardDirection('quer', 'triggerLeft', 'quer'), null);
});

test('start cells fill the board column by column', () => {
  assert.deepEqual([1, 10, 11, 20, 21].map((n) => startCell(n, 10)), [
    { col: 0, row: 0 }, { col: 0, row: 9 }, { col: 1, row: 0 }, { col: 1, row: 9 }, { col: 2, row: 0 },
  ]);
  assert.deepEqual(startCell(7, 3), { col: 2, row: 0 });
});

test('the round state of a move round with three players', async () => {
  const { link, game, view } = await board([1, 2, 11]);
  assert.deepEqual(link.cmds('round').map((c) => [c.type, c.phase]), [['move', 'announced'], ['move', 'open']]);
  assert.deepEqual(link.cmds('round').at(-1).keys, { up: 'white', right: 'white', down: 'white', left: 'white' });
  assert.equal(link.cmds('round').at(-1).hold, 'quer');
  assert.equal(game.snapshot().view, 'move');
  assert.deepEqual(view(), {
    roundId: game.round.roundId, id: 'lauf-ins-ziel', title: 'Spiel', question: 'Wer ist zuerst im Ziel?',
    type: 'move', phase: 'open', adopted: false, startAt: game.round.startAt,
    hold: 'quer', goal: 'right', cols: 22, rows: 10,
    results: {
      players: [{ n: 1, col: 0, row: 0, place: null }, { n: 2, col: 0, row: 1, place: null }, { n: 11, col: 1, row: 0, place: null }],
      order: [], winner: null, finished: 0, participants: 3, ready: 0, play: 'alle',
    },
  });
});

test('hold quer, physical keys: each key moves one cell in the turned direction', async () => {
  const { press, cell } = await board([5]); // start (0,4)
  press(5, 'up');
  assert.deepEqual(cell(5), [1, 4]); // up is right
  press(5, 'right');
  assert.deepEqual(cell(5), [1, 5]); // right is down
  press(5, 'left');
  assert.deepEqual(cell(5), [1, 4]); // left is up
  press(5, 'down');
  assert.deepEqual(cell(5), [0, 4]); // down is left
  press(5, 'buzz', 'enter');
  assert.deepEqual(cell(5), [0, 4]);
});

test('hold hochkant, physical keys: the keys move as printed', async () => {
  const { press, cell } = await board([5], { hold: 'hochkant' });
  press(5, 'right', 'right', 'down', 'up', 'up', 'left');
  assert.deepEqual(cell(5), [1, 3]);
});

test('no move past the border', async () => {
  const { press, cell } = await board([1, 10], { cols: 3 });
  press(1, 'left', 'down'); // up and left from (0,0)
  assert.deepEqual(cell(1), [0, 0]);
  press(10, 'right'); // down from the last row
  assert.deepEqual(cell(10), [0, 9]);
});

test('no move onto an occupied cell', async () => {
  const { press, cell } = await board([1, 2, 11]);
  press(1, 'right'); // down onto 2
  press(1, 'up'); // right onto 11
  assert.deepEqual(cell(1), [0, 0]);
  press(2, 'up'); // 2 goes right to (1,1)
  press(1, 'right'); // now 1 can go down
  assert.deepEqual(cell(1), [0, 1]);
  press(11, 'right'); // down onto 2 at (1,1)
  assert.deepEqual(cell(11), [1, 0]);
});

test('the goal column: finish order, winner, finished devices stay put', async () => {
  const { press, cell, view } = await board([1, 2, 3], { cols: 3 });
  press(2, 'up', 'up');
  press(1, 'up');
  press(1, 'up');
  let res = view().results;
  assert.deepEqual(res.order, [2, 1]);
  assert.equal(res.winner, 2);
  assert.equal(res.finished, 2);
  assert.deepEqual(res.players.map((p) => p.place), [2, 1, null]);
  press(2, 'down', 'right'); // finished: ignored
  assert.deepEqual(cell(2), [2, 1]);
  press(3, 'up', 'up');
  res = view().results;
  assert.deepEqual(res.order, [2, 1, 3]);
  assert.deepEqual(res.players, [{ n: 1, col: 2, row: 0, place: 2 }, { n: 2, col: 2, row: 1, place: 1 }, { n: 3, col: 2, row: 2, place: 3 }]);
});

test('presses count only while open; close freezes the board', async () => {
  const { game, press, cell } = await board([1], {}, { open: false });
  press(1, 'up');
  assert.deepEqual(cell(1), [0, 0]); // announced
  await game.act('open');
  press(1, 'up');
  assert.deepEqual(cell(1), [1, 0]);
  await game.act('close');
  press(1, 'up');
  assert.deepEqual(cell(1), [1, 0]);
  assert.equal(game.snapshot().round.phase, 'closed');
});

test('opening again resets everybody to the start cells', async () => {
  const { game, press, cell, seq, view } = await board([1, 2], { cols: 3 });
  press(1, 'up', 'up');
  press(2, 'up');
  assert.deepEqual(view().results.order, [1]);
  await game.act('close');
  await game.act('announce', { id: 'lauf-ins-ziel' });
  assert.deepEqual([cell(1), cell(2)], [[0, 0], [0, 1]]);
  await game.act('open');
  seq.clear(); // a new round: the buzzers count from 1 again
  const res = view().results;
  assert.deepEqual([cell(1), cell(2)], [[0, 0], [0, 1]]);
  assert.deepEqual([res.order, res.winner, res.finished], [[], null, 0]);
  press(2, 'up');
  assert.deepEqual(cell(2), [1, 1]);
});

test('a device admitted later gets its start cell, once that cell is free', async () => {
  const { link, press, cell, view } = await board([1]);
  link.emitEvent(join(MAC(3), 3, { pressed: false, rejoin: true }));
  await flush();
  assert.deepEqual(cell(3), [0, 2]);
  press(1, 'right'); // 1 goes down onto (0,1), the start cell of 2
  link.emitEvent(join(MAC(2), 2, { pressed: false, rejoin: true }));
  await flush();
  assert.deepEqual(view().results.players.find((p) => p.n === 2), { n: 2, col: null, row: null, place: null });
  assert.equal(view().results.participants, 3);
  press(2, 'up'); // no cell, no move
  press(1, 'up'); // 1 goes right
  assert.deepEqual([cell(1), cell(2)], [[1, 1], [0, 1]]);
});

test('a start cell beyond the board or in the goal column gives no figure', async () => {
  const { press, view } = await board([1, 2, 3], { cols: 2, rows: 1 });
  assert.deepEqual(view().results.players.map((p) => [p.n, p.col, p.row]), [[1, 0, 0], [2, null, null], [3, null, null]]);
  press(1, 'up');
  assert.deepEqual(view().results.order, [1]);
});

test('a released device leaves the board and frees its cell', async () => {
  const { game, press, cell, view } = await board([1, 2]);
  await game.act('release', { n: 2 });
  assert.deepEqual(view().results.players.map((p) => p.n), [1]);
  press(1, 'right');
  assert.deepEqual(cell(1), [0, 1]);
});

test('key lines: a resend is applied once, a joining device counts from 1 again', async () => {
  const { link, keyEv, cell } = await board([1]);
  link.emitEvent(keyEv(1, 'up', 1));
  link.emitEvent(keyEv(1, 'up', 1)); // resend
  assert.deepEqual(cell(1), [1, 0]);
  link.emitEvent(keyEv(1, 'up', 3)); // 2 got lost on the way
  link.emitEvent(keyEv(1, 'up', 2)); // older
  assert.deepEqual(cell(1), [2, 0]);
  for (const s of [100, 200, 255, 0]) link.emitEvent(keyEv(1, 'up', s)); // 8 bit, wraps after 255
  assert.deepEqual(cell(1), [6, 0]);
  link.emitEvent(keyEv(1, 'up', 0));
  link.emitEvent(keyEv(1, 'up', 250));
  assert.deepEqual(cell(1), [6, 0]);
  link.emitEvent(join(MAC(1), 1, { pressed: false, rejoin: true })); // the buzzer restarted
  await flush();
  link.emitEvent(keyEv(1, 'up', 1));
  assert.deepEqual(cell(1), [7, 0]);
  link.emitEvent({ ...keyEv(1, 'up', 2), round: 4 }); // another round
  link.emitEvent({ ...keyEv(1, 'up', 3), kseq: undefined });
  assert.deepEqual(cell(1), [7, 0]);
});
