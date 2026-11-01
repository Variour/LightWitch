import { test } from 'node:test';
import assert from 'node:assert/strict';
import { GameConsole, raceRanking, TIMING } from '../src/game.js';
import { FakeLink, join, value, device, flush, ROUNDS, sleep } from './helpers.js';
import { validateInput } from '../src/validate.js';

const MAC = (i) => `02:00:00:00:00:${i.toString(16).padStart(2, '0')}`;

function setup(opts = {}) {
  const link = new FakeLink(opts.reply);
  const game = new GameConsole({ link, autoAdmit: opts.autoAdmit ?? true, random: () => 0.5 });
  return { link, game };
}

async function withSession(opts) {
  const s = setup(opts);
  const r = await s.game.act('session-start');
  assert.equal(r.ok, true);
  return s;
}

const load = (game, ...rounds) => game.load(validateInput({ format: 'buzzer-session/0', title: 'Test', rounds }));

test('a joining device keeps its stored number; a taken one gets the smallest free', async () => {
  const { link, game } = await withSession();
  link.emitEvent(join(MAC(1), 17));
  link.emitEvent(join(MAC(2), 17));
  link.emitEvent(join(MAC(3), null));
  await flush();
  assert.deepEqual(link.cmds('admit').map((c) => [c.mac, c.n]), [[MAC(1), 17], [MAC(2), 1], [MAC(3), 2]]);
  const snap = game.snapshot();
  assert.deepEqual(snap.participants.map((p) => p.n), [1, 2, 17]);
  assert.equal(snap.counts.admitted, 3);
});

test('repeated join lines of one device lead to one admit at a time', async () => {
  const { link } = await withSession({ reply: (c) => (c.cmd === 'admit' ? null : { ns: 'game', ev: 'session', active: true }) });
  link.emitEvent(join(MAC(1), 5));
  link.emitEvent(join(MAC(1), 5));
  link.emitEvent(join(MAC(1), 5));
  assert.equal(link.cmds('admit').length, 1);
});

test('manual admission: devices wait until the presenter admits them', async () => {
  const { link, game } = await withSession({ autoAdmit: false });
  link.emitEvent(join(MAC(1), 4));
  link.emitEvent(join(MAC(2), null));
  assert.equal(link.cmds('admit').length, 0);
  assert.equal(game.snapshot().counts.waiting, 2);
  const r = await game.act('admit', { mac: 'all' });
  assert.equal(r.ok, true);
  await flush();
  assert.deepEqual(link.cmds('admit').map((c) => c.n).sort(), [1, 4]);
  assert.equal(game.snapshot().counts.waiting, 0);
});

test('with login closed only known devices and rejoins are admitted', async () => {
  const { link, game } = await withSession();
  link.emitEvent(join(MAC(1), 3));
  await flush();
  await game.act('login-close');
  link.emitEvent(join(MAC(9), 9)); // a stranger
  link.emitEvent(join(MAC(1), 3, { pressed: false, rejoin: true })); // restarted member
  link.emitEvent(join(MAC(7), 7, { pressed: false, rejoin: true })); // member the console never saw
  await flush();
  assert.deepEqual(link.cmds('admit').map((c) => c.mac), [MAC(1), MAC(1), MAC(7)]);
});

test('a status line confirms membership when the admitted line got lost', async () => {
  const { link, game } = await withSession({ reply: (c) => (c.cmd === 'admit' ? null : { ns: 'game', ev: 'x' }) });
  link.emitEvent(join(MAC(1), 12));
  assert.equal(game.snapshot().counts.admitted, 0);
  link.emitEvent(device(12, MAC(1)));
  assert.equal(game.snapshot().counts.admitted, 1);
});

test('renumber and release', async () => {
  const { link, game } = await withSession();
  link.emitEvent(join(MAC(1), 3));
  link.emitEvent(join(MAC(2), 4));
  await flush();
  assert.equal((await game.act('renumber', { n: 3, to: 4 })).ok, false);
  const r = await game.act('renumber', { n: 3, to: 30 });
  assert.equal(r.ok, true);
  await flush();
  assert.deepEqual(link.cmds('admit').at(-1), { ns: 'game', cmd: 'admit', mac: MAC(1), n: 30 });
  assert.equal((await game.act('release', { n: 4 })).ok, true);
  assert.deepEqual(link.cmds('release').at(-1), { ns: 'game', cmd: 'release', mac: MAC(2) });
  link.emitEvent(device(4, MAC(2))); // last status before it heard the release
  assert.deepEqual(game.snapshot().participants.map((p) => p.n), [30]);
});

test('value lines are not filtered by vseq; late values do not count', async () => {
  const { link, game } = await withSession();
  load(game, ROUNDS.scale);
  link.emitEvent(join(MAC(1), 1));
  link.emitEvent(join(MAC(2), 2));
  await game.act('next'); // announce (closes login)
  await game.act('next'); // open
  const id = game.round.roundId;
  link.emitEvent(value(1, id, 7, { vseq: 5 }));
  link.emitEvent(value(1, id, 2, { vseq: 1 })); // after a rejoin the device counts from 1
  link.emitEvent(value(2, id, 9, { vseq: 1, late: true }));
  const res = game.snapshot().round.results;
  assert.deepEqual(res.entries, [{ n: 1, v: 2, zone: 'red' }]);
  assert.equal(res.voters, 1);
  assert.equal(res.mean, 2);
  assert.equal(res.counts.find((c) => c.v === 2).count, 1);
});

test('values for the previous round still land there after the next one started', async () => {
  const { link, game } = await withSession();
  load(game, ROUNDS.scale, ROUNDS.idle);
  await game.act('next');
  await game.act('next');
  const first = game.round;
  await game.act('next'); // close
  await game.act('next'); // pause, after the 1 s Nachlauf
  link.emitEvent(value(3, first.roundId, 8));
  assert.equal(first.values.get(3).v, 8);
  assert.notEqual(game.round, first);
});

test('race ranking: by press moment, ties under 5 ms, arrival breaks exact ties', () => {
  const run = { values: new Map([
    [17, { t: 409, arr: 2, late: false }],
    [18, { t: 412, arr: 1, late: false }],
    [19, { t: 414, arr: 3, late: false }],
    [20, { t: 409, arr: 4, late: false }],
    [21, { t: 300, arr: 5, late: true }],
    [22, { t: 600, arr: 6, late: false }],
  ]) };
  assert.deepEqual(raceRanking(run).map((e) => [e.n, e.rank, e.tie]), [
    [17, 1, true], [20, 1, true], [18, 1, true], [19, 4, false], [22, 5, false],
  ]);
});

test('race: winner 1 s after the first press, corrected by an earlier press, manual wins', async () => {
  const { link, game } = await withSession();
  load(game, ROUNDS.race);
  await game.act('next');
  const r = await game.act('next');
  assert.match(r.message, /Freigabe in 1 s/);
  assert.equal(link.cmds('round').at(-1).openInMs, TIMING.raceOpenInMs);
  const id = game.round.roundId;
  link.emitEvent(value(18, id, 1, { t: 412, arr: 1 }));
  link.emitEvent(value(17, id, 1, { t: 430, arr: 2 }));
  assert.equal(game.round.winner, 0);
  await sleep(TIMING.raceWinnerMs + 50);
  assert.equal(game.round.winner, 18);
  assert.equal(link.cmds('round').at(-1).winner, 18);
  link.emitEvent(value(19, id, 1, { t: 400, arr: 3 }));
  assert.equal(game.round.winner, 19);
  await game.act('winner', { n: 17 });
  link.emitEvent(value(20, id, 1, { t: 100, arr: 4 }));
  assert.equal(game.round.winner, 17);
  assert.equal(link.cmds('round').at(-1).winner, 17);
});

test('choice: only the count while open, distribution after closing, then the reveal', async () => {
  const { link, game } = await withSession();
  load(game, ROUNDS.choice);
  await game.act('next');
  await game.act('next');
  const id = game.round.roundId;
  link.emitEvent(value(1, id, 1));
  link.emitEvent(value(2, id, 2));
  link.emitEvent(value(3, id, 1));
  let v = game.snapshot().round;
  assert.deepEqual(v.results, { answered: 3, participants: 0 });
  assert.equal(v.correct, null);
  await game.act('next'); // close
  v = game.snapshot().round;
  assert.deepEqual(v.results.counts, [2, 1, 0, 0]);
  assert.equal('correct' in link.cmds('round').at(-1), false);
  await game.act('next'); // reveal
  v = game.snapshot().round;
  assert.equal(v.correct, 1);
  assert.deepEqual(v.results.right, [1, 3]);
  assert.deepEqual(link.cmds('round').at(-1), { ns: 'game', cmd: 'round', round: id, type: 'choice', phase: 'closed', keys: { up: 'yellow', right: 'blue', down: 'magenta', left: 'cyan' }, correct: 1 });
});

test('next walks through session, login, rounds and keeps the 1 s Nachlauf', async () => {
  const { link, game } = setup();
  load(game, ROUNDS.scale, ROUNDS.race);
  const steps = [];
  for (let i = 0; i < 6; i++) steps.push((await game.act('next')).message);
  assert.match(steps[0], /Sitzung .* gestartet/);
  assert.match(steps[1], /Angekündigt: Runde 4/);
  assert.match(steps[2], /Runde offen/);
  assert.match(steps[3], /geschlossen/);
  assert.match(steps[4], /Angekündigt: Schnellrunde/);
  assert.match(steps[5], /Freigabe/);
  const sessions = link.cmds('session');
  assert.equal(sessions[0].login, true);
  assert.equal(sessions[1].login, false); // announcing closes login
  const rounds = link.cmds('round');
  assert.deepEqual(rounds.map((c) => [c.type, c.phase]), [['scale', 'announced'], ['scale', 'open'], ['scale', 'closed'], ['race', 'announced'], ['race', 'open']]);
  assert.notEqual(rounds[0].round, rounds[3].round);
  const closedAt = game.prevRound.closedAt;
  assert.ok(game.lastCloseAt === closedAt);
  await game.act('next'); // close the race
  assert.match((await game.act('next')).message, /Keine weitere Runde geplant\. Ruhe\./);
  assert.equal(link.cmds('round').at(-1).type, 'idle');
  const last = await game.act('next');
  assert.equal(last.ok, false);
  assert.match(last.error, /Keine weitere Runde/);
});

test('the next round waits 1 s after closing', async () => {
  const { game } = await withSession();
  load(game, ROUNDS.scale, ROUNDS.race);
  await game.act('next');
  await game.act('next');
  const t0 = Date.now();
  await game.act('next'); // close
  await game.act('next'); // announce the race
  assert.ok(Date.now() - t0 >= TIMING.afterCloseMs - 5);
});

test('rounds from the skill queue up as next, in order; the same id replaces', async () => {
  const { game } = setup();
  load(game, ROUNDS.scale, ROUNDS.race);
  game.load(validateInput({ ...ROUNDS.choice, id: 'a' }));
  game.load(validateInput({ ...ROUNDS.choice, id: 'b' }));
  assert.deepEqual(game.plan.rounds.map((r) => r.id), ['a', 'b', 'runde-4', 'schnellrunde']);
  const r = game.load(validateInput({ ...ROUNDS.race, title: 'Neu' }));
  assert.match(r.message, /ersetzt/);
  assert.equal(game.plan.rounds[3].title, 'Neu');
  assert.equal(game.plan.rounds.length, 4);
});

test('replay: the gateway lost the session, the console sends session and round again', async () => {
  const { link, game } = await withSession();
  load(game, ROUNDS.scale);
  await game.act('next');
  await game.act('next');
  const before = link.sent.length;
  link.emitEvent({ ns: 'game', ev: 'session', session: 0, channel: 0, fleet: 0, login: false, active: false });
  const again = link.sent.slice(before);
  assert.deepEqual(again.map((c) => c.cmd), ['session', 'round']);
  assert.deepEqual(link.batches.at(-1), again); // one write, read in one gateway pass
  assert.equal(again[1].round, game.round.roundId);
  assert.equal(again[1].phase, 'open');
  assert.match(game.snapshot().warning, /neu gestartet/);
});

test('replay: a gateway in another round gets the console round again', async () => {
  const { link, game } = await withSession();
  load(game, ROUNDS.scale);
  await game.act('next');
  const before = link.sent.length;
  link.emitEvent({ ns: 'game', ev: 'session', session: game.session.id, channel: 6, fleet: 0, login: false, active: true });
  link.emitEvent({ ns: 'game', ev: 'state', round: game.round.roundId, seq: 1, type: 'scale', phase: 'announced', changed: false, sinceOpenMs: null, closedAtMs: null });
  assert.equal(link.sent.length, before);
  link.emitEvent({ ns: 'game', ev: 'state', round: 0, seq: 1, type: 'idle', phase: null, changed: false, sinceOpenMs: null, closedAtMs: null });
  assert.equal(link.sent.at(-1).cmd, 'round');
});

test('replay: a console without session takes over the running one with its devices and values', async () => {
  const { link, game } = setup();
  link.emitEvent({ ns: 'game', ev: 'session', session: 4711, channel: 6, fleet: 0, login: false, active: true });
  link.emitEvent({ ns: 'game', ev: 'state', round: 23417, seq: 2, type: 'scale', phase: 'open', changed: false, sinceOpenMs: 5000, closedAtMs: null });
  link.emitEvent(device(17, MAC(17)));
  link.emitEvent(value(17, 23417, 7));
  const s = game.snapshot();
  assert.equal(s.session.id, 4711);
  assert.equal(s.view, 'scale');
  assert.equal(s.counts.admitted, 1);
  assert.deepEqual(s.round.results.entries, [{ n: 17, v: 7, zone: 'green' }]);
  assert.equal(link.sent.length, 0);
  assert.equal((await game.act('close')).ok, false); // title, keys and scale are unknown
  const next = await game.act('next');
  assert.equal(next.ok, true);
  assert.equal(link.sent.at(-1).type, 'idle');
});

test('end: the session stays until the gateway reports it inactive', async () => {
  const { link, game } = await withSession({ reply: (c) => (c.cmd === 'end' ? null : { ns: 'game', ev: 'x' }) });
  await game.act('end');
  assert.equal(game.snapshot().session.ending, true);
  assert.equal((await game.act('next')).ok, false);
  link.emitEvent({ ns: 'game', ev: 'session', id: 99, session: 1, channel: 6, fleet: 0, login: false, active: false });
  const s = game.snapshot();
  assert.equal(s.session, null);
  assert.equal(s.view, 'start');
  assert.equal(s.ended, true);
});

test('actions without a session explain what to do', async () => {
  const { game } = setup();
  const r = await game.act('open');
  assert.deepEqual(r, { ok: false, error: 'Keine Sitzung. Erst starten.' });
  assert.equal((await game.act('fly')).ok, false);
});
