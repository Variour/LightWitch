import { test } from 'node:test';
import assert from 'node:assert/strict';
import { GameConsole } from '../src/game.js';
import { LAGEN, toLogical, toPhysical, requiredLage } from '../src/lage.js';
import { validateInput } from '../src/validate.js';
import { FakeLink, join, device, value, flush, ROUNDS } from './helpers.js';

const MAC = (i) => `02:00:00:00:00:${i.toString(16).padStart(2, '0')}`;

// A session with these participants ({ n: lage }, null: no lage in the
// line) and the round announced.
async function setup(lagen, round) {
  const link = new FakeLink();
  const game = new GameConsole({ link, random: () => 0.5 });
  await game.act('session-start');
  for (const [n, lage] of Object.entries(lagen)) link.emitEvent(join(MAC(Number(n)), Number(n), lage ? { lage } : {}));
  await flush();
  game.load(validateInput(round));
  await game.act('next'); // announce
  const turn = (n, lage) => link.emitEvent(device(n, MAC(n), { lage }));
  const view = () => game.snapshot().round;
  return { link, game, turn, view };
}

test('the button for each logical input, per lage', () => {
  const row = (lage) => ['up', 'right', 'down', 'left', 'triggerLeft', 'triggerRight'].map((k) => toPhysical(lage, k));
  assert.deepEqual(row('hochkant'), ['up', 'right', 'down', 'left', null, null]);
  assert.deepEqual(row('quer'), ['left', 'up', 'right', 'down', 'l2', 'l1']);
  assert.deepEqual(row('quer-links'), ['right', 'down', 'left', 'up', 'r1', 'r2']);
  assert.deepEqual(row('liegend'), ['up', 'right', 'down', 'left', null, null]);
  for (const lage of LAGEN) {
    assert.deepEqual([toPhysical(lage, 'buzzer'), toPhysical(lage, 'enter')], ['buzz', 'enter'], lage);
    for (const k of ['up', 'right', 'down', 'left', 'buzzer', 'enter']) assert.equal(toLogical(lage, toPhysical(lage, k)), k, `${lage} ${k}`);
  }
  assert.equal(toLogical('quer', 'l1'), 'triggerRight');
  assert.equal(toLogical('hochkant', 'l1'), null);
  assert.deepEqual(['frei', 'hochkant', 'quer', 'liegend', 'fest'].map(requiredLage), [null, 'hochkant', 'quer', 'liegend', null]);
});

test('participants carry their lage from join and device lines', async () => {
  const { link, game, turn } = await setup({ 1: 'quer', 2: null }, ROUNDS.race);
  const lagen = () => game.snapshot().participants.map((p) => [p.n, p.lage]);
  assert.deepEqual(lagen(), [[1, 'quer'], [2, null]]);
  turn(2, 'liegend');
  turn(1, 'kopfueber'); // no lage the console knows
  assert.deepEqual(lagen(), [[1, null], [2, 'liegend']]);
  link.emitEvent(device(2, MAC(2))); // a line without lage keeps it
  assert.deepEqual(lagen(), [[1, null], [2, 'liegend']]);
});

test('a race that requires liegend: hold, ready count, unknown lage is not ready', async () => {
  const { link, game, turn, view } = await setup({ 1: 'liegend', 2: 'quer', 3: null }, { ...ROUNDS.race, hold: 'liegend' });
  assert.equal(link.cmds('round').at(-1).hold, 'liegend');
  assert.deepEqual([view().hold, view().phase, view().results.ready, view().results.participants], ['liegend', 'announced', 1, 3]);
  turn(2, 'liegend');
  assert.equal(view().results.ready, 2);
  turn(1, 'hochkant');
  assert.equal(view().results.ready, 1);
  await game.act('open');
  assert.equal(view().results.ready, 1);
});

test('frei and fest have no ready count', async () => {
  for (const hold of ['frei', 'fest']) {
    const { view } = await setup({ 1: 'liegend' }, { ...ROUNDS.scale, hold });
    assert.deepEqual([view().hold, view().results.ready], [hold, null]);
  }
  const { view } = await setup({ 1: 'liegend' }, ROUNDS.choice);
  assert.deepEqual([view().hold, view().results.ready], ['frei', null]);
});

test('inputs in another lage than required do not count; unknown lage passes', async () => {
  const { link, game, view } = await setup({ 1: 'liegend', 2: 'quer', 3: null, 4: 'quer' }, { ...ROUNDS.race, hold: 'liegend' });
  await game.act('open');
  const id = game.round.roundId;
  link.emitEvent(value(1, id, 1, { t: 300, arr: 1, lage: 'liegend' }));
  link.emitEvent(value(2, id, 1, { t: 200, arr: 2, lage: 'quer' })); // its own lage says no
  link.emitEvent(value(4, id, 1, { t: 250, arr: 3, lage: 'liegend' })); // its own lage decides
  link.emitEvent(value(3, id, 1, { t: 400, arr: 4 })); // today's firmware
  assert.deepEqual(view().results.ranking.map((e) => e.n), [4, 1, 3]);
  link.emitEvent(value(2, id, 1, { t: 100, arr: 5 })); // no lage in the line: the participant's
  assert.deepEqual(view().results.ranking.map((e) => e.n), [4, 1, 3]);
});

test('scale and choice values are gated the same way', async () => {
  for (const [round, v] of [[ROUNDS.scale, 7], [ROUNDS.choice, 2]]) {
    const { link, game, view } = await setup({ 1: 'hochkant', 2: 'quer' }, { ...round, hold: 'hochkant' });
    await game.act('open');
    const id = game.round.roundId;
    link.emitEvent(value(1, id, v, { lage: 'hochkant' }));
    link.emitEvent(value(2, id, v, { lage: 'quer' }));
    const res = view().results;
    assert.equal(round.type === 'scale' ? res.voters : res.answered, 1, round.type);
  }
});

test('move: keys with a lage are logical, gated by the required lage', async () => {
  const { link, game, view } = await setup({ 1: 'quer', 2: 'liegend', 3: null }, ROUNDS.move);
  await game.act('open');
  const id = game.round.roundId;
  const key = (n, k, kseq, extra = {}) => link.emitEvent({ ns: 'game', ev: 'key', n, round: id, key: k, kseq, ...extra });
  const cell = (n) => {
    const p = view().results.players.find((x) => x.n === n);
    return [p.col, p.row];
  };
  key(1, 'right', 1, { lage: 'quer' }); // logical: right, not turned again
  key(3, 'up', 1); // physical up on a quer buzzer: right
  assert.deepEqual([cell(1), cell(3)], [[1, 0], [1, 2]]);
  key(2, 'right', 1, { lage: 'liegend' }); // the buzzer lies: no move
  key(2, 'right', 2); // no lage in the line: the participant's
  assert.deepEqual(cell(2), [0, 1]);
  key(2, 'right', 2, { lage: 'quer' }); // a resend of kseq 2: the gated press counted as seen
  assert.deepEqual(cell(2), [0, 1]);
  key(2, 'right', 3, { lage: 'quer' });
  assert.deepEqual(cell(2), [1, 1]);
  assert.equal(view().results.ready, 1);
});

test('move with hold frei: logical keys of any lage, physical keys as printed', async () => {
  const { link, game, view } = await setup({ 1: 'liegend', 2: null }, { ...ROUNDS.move, hold: 'frei' });
  await game.act('open');
  const id = game.round.roundId;
  link.emitEvent({ ns: 'game', ev: 'key', n: 1, round: id, key: 'right', kseq: 1, lage: 'liegend' });
  link.emitEvent({ ns: 'game', ev: 'key', n: 2, round: id, key: 'right', kseq: 1 });
  assert.deepEqual(view().results.players.map((p) => [p.n, p.col]), [[1, 1], [2, 1]]);
  assert.equal(view().results.ready, null);
});

test('fest: the lage at the open stays the round\'s lage', async () => {
  const { link, game, turn } = await setup({ 1: 'quer', 2: 'liegend' }, { ...ROUNDS.scale, hold: 'fest' });
  turn(1, 'hochkant'); // before the open: counts
  await game.act('open');
  const r = game.round;
  turn(1, 'liegend');
  turn(2, 'quer');
  link.emitEvent(join(MAC(3), 3, { lage: 'quer', pressed: false, rejoin: true })); // admitted later
  await flush();
  assert.deepEqual([1, 2, 3].map((n) => game._lageOf(r, n)), ['hochkant', 'liegend', 'quer']);
  turn(3, 'liegend');
  assert.equal(game._lageOf(r, 3), 'quer');
  assert.deepEqual(game.snapshot().participants.map((p) => p.lage), ['liegend', 'quer', 'liegend']); // live lage
  assert.equal(game.snapshot().round.results.ready, null);
});
