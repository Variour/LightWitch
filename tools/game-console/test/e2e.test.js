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
import { ROUNDS, waitFor, sleep } from './helpers.js';

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
    assert.match(got.stdout, /Firmware 1a2b3c4d, Nummer keine, 1 WLAN-Netz\(e\), Ladeziel Lagern/);
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
