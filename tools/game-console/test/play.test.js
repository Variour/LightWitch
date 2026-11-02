import { test } from 'node:test';
import assert from 'node:assert/strict';
import { GameConsole } from '../src/game.js';
import { groupOf, teamStart } from '../src/move.js';
import { validateInput } from '../src/validate.js';
import { FakeLink, join, flush, ROUNDS } from './helpers.js';

const MAC = (i) => `02:00:00:00:${(i >> 8).toString(16).padStart(2, '0')}:${(i & 0xff).toString(16).padStart(2, '0')}`;

// A session with these numbers admitted and a move round with hold "frei"
// (keys as printed), open unless told otherwise.
async function board(numbers, round, { open = true } = {}) {
  const link = new FakeLink();
  const game = new GameConsole({ link, random: () => 0.5 });
  await game.act('session-start');
  for (const n of numbers) link.emitEvent(join(MAC(n), n));
  await flush();
  game.load(validateInput({ ...ROUNDS.move, hold: 'frei', ...round }));
  await game.act('next'); // announce
  if (open) await game.act('next');
  const seq = new Map();
  const press = (n, ...keys) => {
    for (const k of keys) {
      const s = ((seq.get(n) ?? 0) + 1) & 0xff;
      seq.set(n, s);
      link.emitEvent({ ns: 'game', ev: 'key', n, round: game.round.roundId, key: k, kseq: s });
    }
  };
  const admit = async (n) => {
    link.emitEvent(join(MAC(n), n, { pressed: false, rejoin: true }));
    await flush();
  };
  const res = () => game.snapshot().round.results;
  const cell = (n) => {
    const p = res().players.find((x) => x.n === n);
    return [p.col, p.row];
  };
  return { link, game, press, admit, res, cell, seq };
}

test('groups are transport cases of 10, teams start in every second row', () => {
  assert.deepEqual([1, 10, 11, 20, 21, 100].map(groupOf), [1, 1, 2, 2, 3, 10]);
  assert.deepEqual([1, 2, 3].map((k) => teamStart(k, 10)), [{ x: 0, y: 0 }, { x: 0, y: 2 }, { x: 0, y: 4 }]);
  assert.deepEqual(teamStart(6, 10), { x: 0, y: 9 });
});

test('team: one figure per group, each press moves it by 1/members', async () => {
  const { press, res, game } = await board([1, 2, 3, 11, 25], { play: 'team' });
  let r = res();
  assert.equal(r.play, 'team');
  assert.deepEqual(r.teams, [
    { team: 1, members: 3, x: 0, y: 0, place: null },
    { team: 2, members: 1, x: 0, y: 2, place: null },
    { team: 3, members: 1, x: 0, y: 4, place: null },
  ]);
  assert.ok(r.players.every((p) => p.col === null && p.row === null && p.place === null));
  press(1, 'right');
  assert.equal(res().teams[0].x, 0.333);
  press(2, 'right');
  press(3, 'right');
  assert.deepEqual([res().teams[0].x, res().teams[0].y], [1, 0]);
  press(1, 'up'); // clamped at the top
  press(2, 'down', 'down', 'down', 'down', 'down', 'down'); // 2 cells down: onto team 2, no blocking
  r = res();
  assert.deepEqual([r.teams[0].x, r.teams[0].y, r.teams[1].y], [1, 2, 2]);
  press(11, 'left'); // clamped at the left
  assert.equal(res().teams[1].x, 0);
  assert.equal(game.moveRole(11), 'play');
});

test('team: finish order and winner are team numbers, finished teams stop', async () => {
  const { press, res, game } = await board([1, 2, 11, 25], { play: 'team', cols: 3 });
  press(11, 'right', 'right'); // one member: a full cell per press
  press(1, 'right', 'right', 'right');
  press(2, 'right'); // team 1 at 4/2 = 2: in the goal
  let r = res();
  assert.deepEqual([r.order, r.winner, r.finished], [[2, 1], 2, 2]);
  assert.deepEqual(r.teams.map((t) => [t.team, t.x, t.place]), [[1, 2, 2], [2, 2, 1], [3, 0, null]]);
  press(11, 'left', 'up');
  r = res();
  assert.deepEqual([r.teams[1].x, r.teams[1].y], [2, 2]);
  assert.deepEqual([game.moveRole(11), game.moveRole(25)], ['out', 'play']);
});

test('team: members count at the moment of the press, new groups join, empty ones leave', async () => {
  const { press, res, admit, game } = await board([1, 2], { play: 'team' });
  press(1, 'right'); // 1/2
  await admit(3);
  await admit(31); // team 4 appears
  press(3, 'right'); // 1/3
  let r = res();
  assert.equal(r.teams[0].x, 0.833);
  assert.deepEqual(r.teams.map((t) => [t.team, t.members, t.x, t.y]), [[1, 3, 0.833, 0], [4, 1, 0, 6]]);
  await game.act('release', { n: 31 });
  r = res();
  assert.deepEqual(r.teams.map((t) => t.team), [1]);
  assert.equal(r.participants, 3);
});

test('team: opening again puts every team back to its start', async () => {
  const { press, res, game, seq } = await board([1, 11], { play: 'team', cols: 3 });
  press(1, 'right', 'right');
  assert.deepEqual(res().order, [1]);
  await game.act('close');
  await game.act('announce', { id: 'lauf-ins-ziel' });
  await game.act('open');
  seq.clear();
  const r = res();
  assert.deepEqual([r.order, r.winner, r.teams.map((t) => [t.x, t.y])], [[], null, [[0, 0], [0, 2]]]);
});

test('erste: the first rows to press get a place in that order, the first press does not move', async () => {
  const numbers = Array.from({ length: 12 }, (_, i) => i + 1);
  const { press, res, cell, game } = await board(numbers, { play: 'erste', rows: 3, cols: 3 });
  let r = res();
  assert.deepEqual([r.play, r.chosen, r.watching], ['erste', [], 12]);
  assert.ok(r.players.every((p) => p.col === null));
  press(5, 'right');
  press(7, 'down');
  assert.deepEqual([cell(5), cell(7)], [[0, 0], [0, 1]]);
  press(5, 'right'); // now it moves
  press(12, 'up');
  press(9, 'right'); // all three places are taken
  press(9, 'right');
  r = res();
  assert.deepEqual([r.chosen, r.watching], [[5, 7, 12], 9]);
  assert.deepEqual([cell(5), cell(7), cell(12), cell(9)], [[1, 0], [0, 1], [0, 2], [null, null]]);
  assert.deepEqual([game.moveRole(5), game.moveRole(9)], ['play', 'out']);
  press(5, 'right');
  r = res();
  assert.deepEqual([r.order, r.winner, r.finished], [[5], 5, 1]);
  assert.equal(r.players.find((p) => p.n === 5).place, 1);
});

test('erste: a place whose start cell is taken waits; wrong lage and the announced phase take none', async () => {
  const { press, cell, res, game, link } = await board([1, 2, 3], { play: 'erste', rows: 3 }, { open: false });
  press(1, 'right'); // announced: nothing
  assert.deepEqual(res().chosen, []);
  await game.act('open');
  press(1, 'right'); // place (0,0)
  press(1, 'down'); // to (0,1), the start of the next place
  press(2, 'up');
  assert.deepEqual([res().chosen, cell(2)], [[1, 2], [null, null]]);
  press(1, 'right');
  assert.deepEqual([cell(1), cell(2)], [[1, 1], [0, 1]]);
  game.round.hold = 'liegend'; // a round that requires lying
  link.emitEvent({ ns: 'game', ev: 'key', n: 3, round: game.round.roundId, key: 'up', kseq: 9, lage: 'quer' });
  assert.deepEqual(res().chosen, [1, 2]);
});

test('gruppen: only the active group moves, the presenter switches, positions stay', async () => {
  const { press, res, cell, game } = await board([3, 12, 14, 25], { play: 'gruppen' });
  assert.deepEqual([res().play, res().group], ['gruppen', 1]);
  assert.deepEqual([cell(3), cell(12), cell(14), cell(25)], [[0, 2], [1, 1], [1, 3], [2, 4]]);
  press(3, 'right');
  press(12, 'right'); // group 2 waits
  assert.deepEqual([cell(3), cell(12)], [[1, 2], [1, 1]]);
  assert.deepEqual([game.moveRole(3), game.moveRole(12)], ['play', 'wait']);
  const r = await game.act('group', { k: 2 });
  assert.deepEqual(r, { ok: true, message: 'Gruppe 2 ist dran.' });
  assert.equal(game.snapshot().round.phase, 'open');
  press(3, 'right');
  press(12, 'right');
  press(14, 'right');
  assert.deepEqual([cell(3), cell(12), cell(14), res().group], [[1, 2], [2, 1], [2, 3], 2]);
});

test('gruppen: the lowest group with members starts; the group action checks its round', async () => {
  const { game, res } = await board([12, 25], { play: 'gruppen' });
  assert.equal(res().group, 2);
  assert.match((await game.act('group', { k: 1 })).error, /^Gruppe 1 hat keine Teilnehmer\.$/);
  assert.match((await game.act('group', { k: 'x' })).error, /^Gruppe: ganze Zahl ab 1\.$/);
  assert.match((await game.act('group', {})).error, /^Gruppe: ganze Zahl ab 1\.$/);
  await game.act('close');
  assert.match((await game.act('group', { k: 3 })).error, /nur, solange die Runde offen ist/);

  const alle = await board([1], {});
  assert.match((await alle.game.act('group', { k: 1 })).error, /nur bei einer Spielfeld-Runde mit "play": "gruppen"/);
  const before = await board([1], { play: 'gruppen' }, { open: false });
  assert.equal(before.res().group, null);
  assert.match((await before.game.act('group', { k: 1 })).error, /nur, solange die Runde offen ist/);
});
