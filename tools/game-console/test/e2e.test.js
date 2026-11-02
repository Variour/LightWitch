// End to end in simulator mode: the real console, link, HTTP server, CLI and
// inbox against the fake gateway with 50 simulated buzzers.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { fileURLToPath } from 'node:url';
import { startConsole } from '../src/app.js';
import { prFirmware } from '../src/sim/power.js';
import { simKeyboard } from '../src/sim/keyboard.js';
import { validateInput } from '../src/validate.js';
import { ROUNDS, waitFor, sleep } from './helpers.js';

const validRound = (r) => validateInput(r).value;

const run = promisify(execFile);
const BIN = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', 'bin', 'game-console.js');

async function start(extra = {}) {
  const inbox = await fs.mkdtemp(path.join(os.tmpdir(), 'buzzer-e2e-'));
  const app = await startConsole({ simulate: 50, httpPort: 0, inbox, simTiming: { endRelease: 1000 }, ...extra });
  const port = app.server.port;
  const api = async (pathname, body, headers = {}) => {
    const res = await fetch(`http://127.0.0.1:${port}${pathname}`, body === undefined ? { headers } : {
      method: 'POST', headers: { 'Content-Type': 'application/json', ...headers }, body: JSON.stringify(body),
    });
    return { status: res.status, body: await res.json() };
  };
  const act = async (action, args = {}) => {
    const r = await api('/api/action', { action, ...args });
    assert.equal(r.body.ok, true, `${action}: ${r.body.error}`);
    return r.body;
  };
  const state = async () => (await api('/api/state')).body;
  return { app, port, inbox, api, act, state, gw: app.transport, game: app.game };
}

test('session, login, 50 admits, scale, race, choice, end', { timeout: 90000 }, async () => {
  const { app, port, inbox, api, act, state, gw, game } = await start();
  try {
    await waitFor(() => app.link.connected, { what: 'link' });

    // Session plan through the CLI.
    const file = path.join(inbox, '..', `sitzung-${port}.json`);
    await fs.writeFile(file, JSON.stringify({ format: 'buzzer-session/0', title: 'Workshop am Nachmittag', rounds: [ROUNDS.scale, ROUNDS.race, ROUNDS.choice] }));
    const cli = await run(process.execPath, [BIN, 'send', file, '--http-port', String(port)]);
    assert.match(cli.stdout, /Sitzung "Workshop am Nachmittag" mit 3 Runde\(n\) geladen/);

    // A rejected round over HTTP, with the German error.
    const bad = await api('/api/load', { ...ROUNDS.race, id: 'x', answers: ['a', 'b'] });
    assert.equal(bad.status, 422);
    assert.match(bad.body.error, /Feld "answers" gehört nicht zum Rundentyp "race"/);

    // The choice round replaced through the inbox.
    await fs.writeFile(path.join(inbox, 'quiz-3.json'), JSON.stringify({ ...ROUNDS.choice, title: 'Quizfrage 3b' }));
    await waitFor(() => game.plan.rounds[2].title === 'Quizfrage 3b', { timeoutMs: 4000, what: 'inbox' });
    assert.deepEqual(await fs.readdir(path.join(inbox, 'done')), ['quiz-3.json']);

    // Login: all 50 join and get the numbers 1-50 (one sticker mix-up resolved).
    await act('next');
    await waitFor(() => game.snapshot().counts.admitted === 50, { timeoutMs: 30000, what: '50 admits' });
    let s = await state();
    assert.equal(s.view, 'login');
    assert.deepEqual(s.participants.map((p) => p.n), Array.from({ length: 50 }, (_, i) => i + 1));
    assert.equal(new Set(s.participants.map((p) => p.mac)).size, 50);
    assert.equal(gw.devices.filter((d) => d.state === 'member').length, 50);

    // Scale.
    await act('next'); // announce, closes login
    s = await state();
    assert.equal(s.view, 'scale');
    assert.equal(s.session.login, false);
    await waitFor(() => gw.login === false, { what: 'login closed on the gateway' });
    await act('next'); // open
    await waitFor(() => game.snapshot().round.results.voters >= 40, { timeoutMs: 15000, what: 'scale votes' });
    await sleep(3000);
    s = await state();
    const sr = s.round.results;
    assert.ok(sr.entries.every((e) => e.v >= 0 && e.v <= 9));
    assert.ok(sr.entries.every((e) => e.zone === (e.v <= 3 ? 'red' : e.v >= 7 ? 'green' : 'yellow')));
    assert.equal(sr.counts.reduce((a, c) => a + c.count, 0), sr.voters);
    // Every shown value is what the device itself holds.
    for (const e of sr.entries) {
      const dev = gw.devices.find((d) => d.number === e.n);
      if (dev.confirmed) assert.equal(dev.val, e.v, `device ${e.n}`);
    }
    await act('next'); // close

    // Race.
    await act('next'); // announce after the Nachlauf
    assert.equal((await state()).view, 'race');
    await act('next'); // open with 1 s countdown
    assert.ok(game.snapshot().round.startAt > Date.now() + 500);
    await waitFor(() => game.snapshot().round.winner, { timeoutMs: 6000, what: 'race winner' });
    await sleep(1500);
    s = await state();
    const ranking = s.round.results.ranking;
    assert.ok(ranking.length >= 40);
    for (let i = 1; i < ranking.length; i++) assert.ok(ranking[i - 1].t <= ranking[i].t);
    assert.equal(s.round.winner, ranking[0].n);
    await waitFor(() => gw.round.winner === ranking[0].n, { what: 'winner on the gateway' });
    await act('next'); // close

    // Choice.
    await act('next');
    await act('next'); // open
    await waitFor(() => game.snapshot().round.results.answered >= 40, { timeoutMs: 15000, what: 'answers' });
    s = await state();
    assert.equal(s.round.results.counts, undefined);
    await act('next'); // close
    s = await state();
    assert.equal(s.round.results.counts.reduce((a, b) => a + b, 0), s.round.results.answered);
    await act('next'); // reveal
    s = await state();
    assert.equal(s.round.correct, 1);
    await waitFor(() => gw.round.correct === 1, { what: 'reveal on the gateway' });
    await waitFor(() => gw.devices.filter((d) => d.round && d.round.correct === 1).length >= 45, { what: 'devices see the reveal' });

    // End: devices are released and back in normal operation.
    await act('end');
    await waitFor(() => game.snapshot().session === null, { timeoutMs: 5000, what: 'session end' });
    assert.equal((await state()).view, 'start');
    assert.equal(gw.devices.filter((d) => d.state !== 'normal').length, 0);
  } finally {
    await app.close();
  }
});

test('lossy radio, a device restart and a gateway restart in the middle of a round', { timeout: 90000 }, async () => {
  const { app, act, gw, game } = await start({ simulate: 20, simLoss: 0.05, simLatency: 30 });
  try {
    await waitFor(() => app.link.connected, { what: 'link' });
    game.load({ kind: 'session', value: { title: '', rounds: [{ ...ROUNDS.scale }] } });
    await act('next');
    await waitFor(() => game.snapshot().counts.admitted === 20, { timeoutMs: 30000, what: '20 admits' });
    await act('next');
    await act('next');
    await waitFor(() => game.snapshot().round.results.voters >= 15, { timeoutMs: 20000, what: 'votes' });

    // A buzzer restarts: it rejoins with its number and votes again, its
    // vseq starting from 1.
    const dev = gw.devices.find((d) => d.state === 'member' && d.val !== null && d.vseq >= 2);
    assert.ok(dev, 'a member with at least two taps');
    const n = dev.number;
    dev.reboot(gw.now());
    await waitFor(() => dev.state === 'member' && dev.round && dev.round.startMs !== undefined, { timeoutMs: 20000, what: 'rejoin' });
    assert.equal(dev.number, n);
    dev.presses = []; // the person holding it waits; the test presses
    dev.press('up', gw.now(), gw.now());
    await waitFor(() => dev.confirmed && dev.vseq === 1, { timeoutMs: 10000, what: 'new value confirmed' });
    assert.equal(game.snapshot().round.results.entries.find((e) => e.n === n).v, dev.val);

    // The gateway restarts: the console sends session and round again in one
    // write. The buzzers stay in the round, keep their values and report them
    // to the new gateway; the next phase change reaches all of them.
    for (const d of gw.devices) d.presses = []; // everyone stops tapping
    await sleep(1000);
    const roundId = game.round.roundId;
    const held = new Map(gw.devices.filter((d) => d.state === 'member' && d.val !== null).map((d) => [d, d.val]));
    assert.ok(held.size >= 15);
    gw.restart();
    await waitFor(() => gw.bActive && gw.round.id === roundId, { timeoutMs: 8000, what: 'gateway restored' });
    assert.equal(gw.session, game.session.id);
    assert.equal(gw.round.phase, 'open');
    assert.match(game.snapshot().warning, /neu gestartet/);
    const onNewGateway = (d, v) => d.round && d.round.id === roundId && d.round.seq === gw.round.seq && d.val === v &&
      d.confirmed && gw.devs.get(d.number) && gw.devs.get(d.number).value === v;
    await waitFor(() => [...held].every(([d, v]) => onNewGateway(d, v)), { timeoutMs: 15000, what: 'values confirmed by the new gateway' });
    await act('close');
    await waitFor(() => [...held.keys()].every((d) => d.round.phase === 'closed'), { timeoutMs: 8000, what: 'close on every buzzer' });
    const entries = game.snapshot().round.results.entries;
    for (const [d, v] of held) assert.equal(entries.find((e) => e.n === d.number).v, v, `console value of ${d.number}`);
  } finally {
    await app.close();
  }
});

// Turning into a required lage, shortened: most within 0.1-0.6 s, then 0.3 s still.
const FAST_TURN = { lageSettleMs: 300, turnMin: 100, turnMax: 600, turnSlowMin: 3000, turnSlowMax: 5000 };

test('move round: the simulated players reach the goal, the keyboard drives one buzzer', { timeout: 90000 }, async () => {
  const FAST_MOVE = { movePaceMin: 60, movePaceMax: 140, moveFirstMin: 50, moveFirstMax: 300 };
  const { app, act, gw, game } = await start({ simulate: 20, simTiming: { endRelease: 1000, ...FAST_MOVE, ...FAST_TURN } });
  const keys = []; // key lines of the keyboard's buzzer
  app.link.on('rx', (ev) => {
    if (ev.ns === 'game' && ev.ev === 'key' && ev.n === 1) keys.push(`${ev.key} ${ev.lage}`);
  });
  try {
    await waitFor(() => app.link.connected, { what: 'link' });
    game.load({ kind: 'round', value: validRound(ROUNDS.move) });
    await act('next');
    await waitFor(() => game.snapshot().counts.admitted === 20, { timeoutMs: 30000, what: '20 admits' });
    const said = [];
    const kb = simKeyboard(app, { say: (t) => said.push(t), quit: () => {} });
    kb.feed('\t');
    assert.deepEqual(said, ['Tastatur steuert Nr. 1']);
    await act('next'); // announce
    await act('next'); // open
    await waitFor(() => gw.round.type === 'move' && gw.round.phase === 'open', { what: 'move round on the gateway' });
    const view = () => game.snapshot().round;
    const at = (n) => view().results.players.find((p) => p.n === n);

    // 20 players, 10 rows: the goal column takes 10.
    await waitFor(() => view().results.finished === 10, { timeoutMs: 40000, what: '10 in the goal' });
    let res = view().results;
    assert.equal(res.players.length, 20);
    assert.equal(res.winner, res.order[0]);
    assert.equal(new Set(res.players.map((p) => `${p.col},${p.row}`)).size, 20);
    for (const p of res.players) {
      assert.equal(p.place === null ? null : res.order[p.place - 1], p.place === null ? null : p.n);
      if (p.place) assert.equal(p.col, 21, `player ${p.n} in the goal`);
    }
    assert.deepEqual([at(1).col, at(1).row], [0, 0]); // the keyboard's buzzer waits
    assert.equal(keys.length, 0);

    // Arrow right: the buzzer sends right in the required lage quer, the
    // figure goes right (a figure still in the way blocks a step).
    for (let i = 0; i < 40 && at(1).col < 5; i++) {
      kb.feed('\x1b[C');
      await sleep(150);
    }
    assert.deepEqual([at(1).col, at(1).row], [5, 0]);
    assert.ok(keys.length >= 5 && keys.every((k) => k === 'right quer'), keys.join());

    assert.equal(gw.devices.find((d) => d.number === 1).lage, 'quer');

    // Tab hands buzzer 1 back to its person.
    kb.feed('\t');
    assert.equal(said.at(-1), 'Tastatur steuert Nr. 2');
    assert.deepEqual(gw.devices.filter((d) => d.manual).map((d) => d.number), [2]);
    await act('close');
    res = view().results;
    await sleep(500);
    assert.deepEqual(view().results, res); // frozen
  } finally {
    await app.close();
  }
});

test('lage: the people turn their buzzers, a race that requires liegend counts only lying presses', { timeout: 90000 }, async () => {
  // More slow and never-turning people than usual, so some stay behind.
  const { app, act, gw, game } = await start({ simulate: 20, simTiming: { endRelease: 1000, ...FAST_TURN, turnSlow: 0.15, turnNever: 0.1 } });
  const values = [];
  app.link.on('rx', (ev) => {
    if (ev.ns === 'game' && ev.ev === 'value') values.push(ev);
  });
  try {
    await waitFor(() => app.link.connected, { what: 'link' });
    game.load({ kind: 'round', value: validRound({ ...ROUNDS.race, hold: 'liegend' }) });
    await act('next');
    await waitFor(() => game.snapshot().counts.admitted === 20, { timeoutMs: 30000, what: '20 admits' });
    await waitFor(() => game.snapshot().participants.every((p) => p.lage !== null), { what: 'every lage known' });
    const members = () => gw.devices.filter((d) => d.state === 'member');
    const lying = () => members().filter((d) => d.lage === 'liegend').length;
    const view = () => game.snapshot().round;

    await act('next'); // announce
    assert.equal(view().hold, 'liegend');
    await waitFor(() => members().every((d) => d.round && d.round.hold === 'liegend'), { what: 'the round on every buzzer' });
    assert.ok(view().results.ready < 20, 'some buzzers do not lie yet');
    await waitFor(() => view().results.ready >= 12, { timeoutMs: 5000, what: 'most turned' });
    const behind = members().filter((d) => d.physLage !== 'liegend' || d.turn);
    assert.ok(behind.length >= 1, 'a few take long or never turn');
    await waitFor(() => view().results.ready === lying(), { what: 'the console follows every new lage' });
    assert.ok(view().results.ready < 20);

    await act('next'); // open
    await waitFor(() => view().winner, { timeoutMs: 6000, what: 'race winner' });
    await sleep(1500);
    const ranking = view().results.ranking;
    assert.ok(ranking.length >= 10);
    assert.ok(values.length >= ranking.length && values.every((v) => v.lage === 'liegend'), 'every press came lying');
    const never = members().filter((d) => d.physLage !== 'liegend').map((d) => d.number);
    assert.equal(ranking.filter((e) => never.includes(e.n)).length, 0);
  } finally {
    await app.close();
  }
});

test('move play modes: team, erste and gruppen with 30 simulated buzzers', { timeout: 120000 }, async () => {
  const FAST_MOVE = { movePaceMin: 60, movePaceMax: 140, moveFirstMin: 50, moveFirstMax: 300 };
  const { app, port, act, gw, game } = await start({ simulate: 30, simTiming: { endRelease: 1000, ...FAST_MOVE } });
  const keyLines = [];
  app.link.on('rx', (ev) => {
    if (ev.ns === 'game' && ev.ev === 'key') keyLines.push({ n: ev.n, at: Date.now() });
  });
  const mode = (play) => ({ ...ROUNDS.move, id: `lauf-${play}`, hold: 'frei', play });
  try {
    await waitFor(() => app.link.connected, { what: 'link' });
    game.load({ kind: 'session', value: { title: '', rounds: ['team', 'erste', 'gruppen'].map((p) => validRound(mode(p))) } });
    await act('next');
    await waitFor(() => game.snapshot().counts.admitted === 30, { timeoutMs: 30000, what: '30 admits' });
    const res = () => game.snapshot().round.results;

    // Team: three teams of ten reach the goal together.
    await act('next');
    await act('next');
    await waitFor(() => res().finished === 3, { timeoutMs: 30000, what: 'three teams in the goal' });
    let r = res();
    assert.deepEqual(r.teams.map((t) => [t.team, t.members, t.x, t.place !== null]), [[1, 10, 21, true], [2, 10, 21, true], [3, 10, 21, true]]);
    assert.deepEqual([...r.order].sort(), [1, 2, 3]);
    assert.equal(r.winner, r.order[0]);
    assert.ok(r.players.every((p) => p.col === null));
    await act('next'); // close

    // Erste: the ten fastest get a place, the others stop pressing.
    await act('next');
    await act('next');
    await waitFor(() => res().chosen.length === 10, { timeoutMs: 8000, what: 'ten places' });
    const fullAt = Date.now();
    r = res();
    assert.equal(new Set(r.chosen).size, 10);
    assert.equal(r.watching, 20);
    await sleep(2000);
    r = res();
    const chosen = new Set(r.chosen);
    assert.ok(r.players.every((p) => (p.col !== null) === chosen.has(p.n)));
    assert.ok(r.players.filter((p) => chosen.has(p.n)).some((p) => p.col > 0), 'the chosen move');
    assert.equal(keyLines.filter((k) => !chosen.has(k.n) && k.at > fullAt + 1000).length, 0, 'watchers stop pressing');
    await act('next'); // close

    // Gruppen: group 1 starts and moves through the waiting groups, the
    // others wait; the keyboard takes only the active group.
    await act('next');
    await act('next');
    assert.equal(res().group, 1);
    await sleep(1500);
    const moved = (p) => p.col !== Math.floor((p.n - 1) / 10) || p.row !== (p.n - 1) % 10;
    const of = (k) => res().players.filter((p) => Math.floor((p.n - 1) / 10) + 1 === k);
    assert.ok(of(1).filter(moved).length >= 5, 'group 1 moves');
    assert.ok(of(1).some((p) => p.col >= 2), 'through the columns of groups 2 and 3');
    assert.ok(of(2).every((p) => !moved(p)) && of(3).every((p) => !moved(p)), 'the other groups wait');
    const said = [];
    const kb = simKeyboard(app, { say: (t) => said.push(t), quit: () => {} });
    kb.feed('\t\t');
    assert.deepEqual(said, ['Tastatur steuert Nr. 1', 'Tastatur steuert Nr. 2']);
    const cli = await run(process.execPath, [BIN, 'group', '2', '--http-port', String(port)]);
    assert.match(cli.stdout, /Gruppe 2 ist dran\./);
    const group1 = JSON.stringify(of(1));
    await sleep(1500);
    assert.equal(res().group, 2);
    assert.ok(of(2).some(moved), 'group 2 moves');
    assert.ok(of(3).every((p) => !moved(p)), 'group 3 waits');
    assert.equal(JSON.stringify(of(1)), group1, 'group 1 keeps its places');
    kb.feed('\x1b[C'); // Nr. 2 cannot move now: the arrow takes the first of group 2
    assert.equal(said.at(-1), 'Tastatur steuert Nr. 11');
    assert.equal(gw.devices.find((d) => d.number === 11).manual, true);
    await act('group', { k: 3 });
    await sleep(1000);
    assert.ok(of(3).some(moved), 'group 3 moves');
    const own = (k) => new Set(of(k).filter((p) => p.col !== null).map((p) => `${p.col},${p.row}`)).size === of(k).length;
    assert.ok([1, 2, 3].every(own), 'no two figures of one group on a cell');
  } finally {
    await app.close();
  }
});

// Fleet timings of the simulator, shortened: updates take 2-4 s, statuses come every ~2 s.
const FAST_FLEET = {
  fleetStatusPeriod: 2000, fleetStatusJitter: 300, fleetFirstStatusMax: 800, fleetUpdateStatusPeriod: 300,
  fleetCheckMin: 200, fleetCheckMax: 400, fleetDownloadMin: 1500, fleetDownloadMax: 3000,
  fleetInstallMin: 200, fleetInstallMax: 400, fleetRebootMs: 300,
};

test('fleet: charge target, update-all, ap, config, wifi-set and off-all against the simulator', { timeout: 90000 }, async () => {
  const logs = [];
  const { app, port, api, act, state, gw } = await start({
    simulate: 12, trace: true, simTiming: { endRelease: 1000, ...FAST_FLEET }, log: (level, text) => logs.push(`${level} ${text}`),
  });
  const cli = (args, input) => {
    const p = run(process.execPath, [BIN, ...args, '--http-port', String(port)]);
    p.child.stdin.end(input ?? '');
    return p;
  };
  try {
    await waitFor(() => app.link.connected, { what: 'link' });
    await waitFor(() => app.game.snapshot().fleet.devices.length === 12, { timeoutMs: 5000, what: '12 devices in the fleet table' });
    let s = await state();
    assert.deepEqual(s.fleet.ref, { fw: s.link.fw, source: 'gateway' });
    assert.equal(s.fleet.counts.outdated, 1);
    assert.equal(s.fleet.counts.errors, 1);
    assert.ok(s.fleet.counts.charging >= 1 && s.fleet.counts.full >= 1);
    for (const d of s.fleet.devices) {
      assert.ok(typeof d.bat === 'number' && typeof d.rssi === 'number' && /^[0-9a-f]{8}$/.test(d.fw), d.mac);
    }

    // Charge target Event: every device reports it, full ones charge on.
    const charge = await cli(['charge', 'event']);
    assert.match(charge.stdout, /Ladeziel Event \(100 %\) an alle gesendet/);
    await waitFor(() => app.game.snapshot().fleet.devices.every((d) => d.target === 'event'), { timeoutMs: 6000, what: 'target event' });
    assert.ok(gw.devices.every((d) => d.power.target === 'event'));
    await waitFor(() => app.game.snapshot().fleet.counts.full === 0, { timeoutMs: 6000, what: 'full devices charge on to 100 %' });

    // Update an alle: the outdated device downloads with progress and comes back with the gateway's firmware.
    await act('update-all');
    await waitFor(() => app.game.snapshot().fleet.devices.some((d) => d.upd === 'downloading' && d.pct > 0), { timeoutMs: 6000, what: 'download progress' });
    await waitFor(() => {
      const f = app.game.snapshot().fleet;
      return f.counts.outdated === 0 && f.counts.updating === 0 && f.devices.length === 12 && f.devices.every((d) => d.fw === f.ref.fw);
    }, { timeoutMs: 15000, what: 'every device on the new firmware' });
    assert.ok(gw.devices.every((d) => d.power.fw === gw.fw));
    assert.equal(app.game.snapshot().link.fwChange, null); // the gateway was on the release already

    // A PR build: the gateway updates itself too, restarts and comes back
    // with the new build; the reference follows and every device is current.
    let drops = 0;
    app.link.on('disconnected', () => {
      drops++;
    });
    const pr = await cli(['update-all', '--pr', '481']);
    assert.match(pr.stdout, /Update an alle gesendet \(PR 481\)/);
    const prFw = prFirmware(481);
    await waitFor(() => drops === 1 && app.link.connected && app.game.snapshot().fleet.ref.fw === prFw, { timeoutMs: 15000, what: 'gateway back on the PR build' });
    const change = app.game.snapshot().link.fwChange;
    assert.deepEqual([change.from, change.to], [gw.world.releaseFw, prFw]);
    await waitFor(() => {
      const f = app.game.snapshot().fleet;
      return f.counts.outdated === 0 && f.counts.updating === 0 && f.devices.length === 12 && f.devices.every((d) => d.fw === prFw);
    }, { timeoutMs: 15000, what: 'every device on the PR build' });

    // Maintenance AP for one device, by MAC; a shared number is refused.
    const one = (await state()).fleet.devices[0];
    await cli(['ap', one.mac]);
    await waitFor(() => gw.devices.find((d) => d.mac === one.mac).power.apUntil > gw.now(), { what: 'AP open' });
    await assert.rejects(cli(['ap', '3']), (e) => e.code === 1 && /mehrere Geräte/.test(e.stderr));

    // The device on the USB cable: WiFi from stdin, then read back.
    const secret = 'Pf3rd-auf-der-Weide';
    const wifi = await cli(['wifi-set'], `Halle 3\n${secret}\n`);
    assert.match(wifi.stdout, /WLAN "Halle 3" auf dem Gerät am USB-Kabel gespeichert/);
    assert.deepEqual(gw.wifi, ['Halle 3']);
    const got = await cli(['config-get']);
    assert.match(got.stdout, new RegExp(`Firmware ${prFw}, Nummer keine, 1 WLAN-Netz\\(e\\), Ladeziel Lagern`));
    assert.ok(logs.some((l) => l.startsWith('tx ') && l.includes('"cmd":"wifi"')), 'the trace shows the command');
    assert.equal(logs.some((l) => l.includes(secret)), false, 'password in the log');
    assert.equal(JSON.stringify(await state()).includes(secret), false, 'password in the state');

    // Off: devices on battery go to ship mode and leave the table; devices on USB stay.
    const onBattery = (await state()).fleet.devices.filter((d) => d.usb === false).map((d) => d.mac).sort();
    assert.equal(onBattery.length, 3);
    const off = await api('/api/action', { action: 'off-all' });
    assert.match(off.body.message, /3 Gerät\(e\) im Akkubetrieb/);
    await sleep(4000); // two status periods
    s = await state();
    assert.equal(s.fleet.devices.length, 9);
    assert.ok(s.fleet.devices.every((d) => d.usb === true));
    assert.deepEqual(gw.devices.filter((d) => d.power.state === 'off').map((d) => d.mac).sort(), onBattery);
  } finally {
    await app.close();
  }
});

test('the API answers only on 127.0.0.1 and only to JSON posts', async () => {
  const { app, port } = await start({ simulate: 1 });
  try {
    const wrongHost = await new Promise((resolve, reject) => {
      http.get({ host: '127.0.0.1', port, path: '/api/state', headers: { Host: `evil.example:${port}` } }, (res) => {
        res.resume();
        resolve(res.statusCode);
      }).on('error', reject);
    });
    assert.equal(wrongHost, 403);
    const res = await fetch(`http://127.0.0.1:${port}/api/action`, { method: 'POST', headers: { 'Content-Type': 'text/plain' }, body: '{"action":"end"}' });
    assert.equal(res.status, 415);
    const page = await fetch(`http://127.0.0.1:${port}/`);
    assert.match(await page.text(), /<title>Buzzer-Bildschirm<\/title>/);
    assert.equal(app.server.url, `http://127.0.0.1:${port}/`);
  } finally {
    await app.close();
  }
});
