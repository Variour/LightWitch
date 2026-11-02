import { test } from 'node:test';
import assert from 'node:assert/strict';
import { GameConsole } from '../src/game.js';
import { FleetTable, parseFleetStatus, errorTexts, FLEET_STALE_MS } from '../src/fleet.js';
import { redact, serialize, cmd } from '../src/protocol.js';
import { FakeLink, fleetStatus, flush } from './helpers.js';

const MAC = (i) => `02:b0:00:00:00:${i.toString(16).padStart(2, '0')}`;

function setup({ reply, spaces } = {}) {
  let t = 1_000_000;
  const clock = { now: () => t, advance: (ms) => { t += ms; } };
  const link = new FakeLink(reply);
  if (spaces) link.hello.spaces = spaces;
  const game = new GameConsole({ link, now: clock.now, random: () => 0.5 });
  return { link, game, clock };
}

const devices = (game) => game.snapshot().fleet.devices;

test('a fleet status line is checked and normalised', () => {
  const s = parseFleetStatus(fleetStatus('02:B0:00:00:00:0A', { n: 0, fw: '4A45FC32', bat: 104, chg: 'boiling', upd: 'downloading', pct: 40, age: 3 }));
  assert.equal(s.mac, '02:b0:00:00:00:0a');
  assert.equal(s.n, null); // 0 = no number
  assert.equal(s.fw, '4a45fc32');
  assert.equal(s.bat, 100);
  assert.equal(s.chg, null); // unknown value: shown as "?"
  assert.equal(s.upd, 'downloading');
  assert.equal(s.ageMs, 3000);
  assert.equal('usbEdges' in parseFleetStatus(fleetStatus(MAC(1), { usbEdges: 3 })), false); // dropped from the protocol
  assert.equal(parseFleetStatus(fleetStatus('nope')), null);
  assert.equal(parseFleetStatus(fleetStatus(MAC(1), { n: null })).n, null);
  assert.equal(parseFleetStatus(fleetStatus(MAC(1), { chg: 'none', bat: null })).chg, 'none'); // no charger
});

test('wifi and updErr: read when present, the reason of a failed update gets a German name', () => {
  const { link, game } = setup();
  link.emitEvent(fleetStatus(MAC(1), { n: 1, wifi: false, upd: 'failed', updErr: 'no_wifi' }));
  link.emitEvent(fleetStatus(MAC(2), { n: 2, wifi: true, updErr: '' }));
  link.emitEvent(fleetStatus(MAC(3), { n: 3, upd: 'failed', updErr: 'something_new' }));
  link.emitEvent(fleetStatus(MAC(4), { n: 4 })); // older firmware without the fields
  const d = devices(game);
  assert.deepEqual(d.map((x) => [x.wifi, x.updErr, x.updErrText]), [
    [false, 'no_wifi', 'kein WLAN'],
    [true, null, null],
    [null, 'something_new', 'something_new'],
    [null, null, null],
  ]);
  assert.equal(game.snapshot().fleet.counts.errors, 2);
});

test('after the gateway restarts with a new build, the reference follows and the change is shown', () => {
  const { link, game, clock } = setup();
  link.emitEvent(fleetStatus(MAC(1), { n: 1, fw: '4a45fc32' }));
  link.emit('hello', link.hello);
  assert.equal(game.snapshot().link.fwChange, null);
  assert.equal(devices(game)[0].fwStatus, 'outdated');
  link.hello = { ...link.hello, fw: '4a45fc32' };
  link.emit('hello', link.hello);
  const snap = game.snapshot();
  assert.deepEqual(snap.fleet.ref, { fw: '4a45fc32', source: 'gateway' });
  assert.equal(snap.fleet.devices[0].fwStatus, 'current');
  assert.deepEqual({ ...snap.link.fwChange, at: 0 }, { from: '1a2b3c4d', to: '4a45fc32', at: 0 });
  assert.match(game.messages.at(-1).text, /neue Firmware 4a45fc32 \(vorher 1a2b3c4d\)/);
  clock.advance(61000);
  assert.equal(game.snapshot().link.fwChange, null);
});

test('error bits get the names of the firmware (FleetTypes.h)', () => {
  assert.deepEqual(errorTexts(0), []);
  assert.deepEqual(errorTexts(0x104), ['Akku zu heiß', 'Akku schwach']);
  assert.deepEqual(errorTexts(0x8001), ['Lader antwortet nicht', 'Fehlerbit 15']);
});

test('the fleet table keeps the latest value per MAC with the moment the bridge heard it', () => {
  const { link, game, clock } = setup();
  link.emitEvent(fleetStatus(MAC(1), { bat: 50, age: 4 }));
  link.emitEvent(fleetStatus(MAC(2), { n: 3, rssi: -70 }));
  const [a, b] = devices(game);
  assert.deepEqual([b.n, a.n], [12, 3]); // sorted by number
  assert.equal(b.lastSeen, clock.now() - 4000);
  assert.equal(b.ageS, 4);
  clock.advance(10000);
  link.emitEvent(fleetStatus(MAC(1), { bat: 55, age: 1 }));
  assert.equal(devices(game)[1].bat, 55);
  // A replay of older news (larger age after a hello) does not go back in time.
  link.emitEvent(fleetStatus(MAC(1), { bat: 50, age: 20 }));
  assert.equal(devices(game)[1].bat, 55);
  // The fields the factory script reads from /api/state.
  const d = devices(game)[1];
  assert.deepEqual({ mac: d.mac, n: d.n, bat: d.bat, fw: d.fw, rssi: d.rssi }, { mac: MAC(1), n: 12, bat: 55, fw: '1a2b3c4d', rssi: -48 });
});

test('counts: charging, full, outdated, updating, errors; silent devices drop out of the counts', () => {
  const { link, game, clock } = setup();
  link.emitEvent(fleetStatus(MAC(1), { n: 1, chg: 'charging' }));
  link.emitEvent(fleetStatus(MAC(2), { n: 2, chg: 'full', bat: 80 }));
  link.emitEvent(fleetStatus(MAC(3), { n: 3, chg: 'fault', err: 1 }));
  link.emitEvent(fleetStatus(MAC(4), { n: 4, fw: '0e9d8c7b', upd: 'downloading', pct: 30 }));
  link.emitEvent(fleetStatus(MAC(5), { n: 5, state: 'active', usb: false, chg: 'discharging', upd: 'failed' }));
  let c = game.snapshot().fleet.counts;
  assert.deepEqual(c, { total: 5, charging: 2, full: 1, outdated: 1, updating: 1, errors: 2, silent: 0 });
  clock.advance(FLEET_STALE_MS - 1000);
  link.emitEvent(fleetStatus(MAC(1), { n: 1, chg: 'charging' }));
  clock.advance(2000);
  c = game.snapshot().fleet.counts;
  assert.deepEqual(c, { total: 5, charging: 1, full: 0, outdated: 0, updating: 0, errors: 0, silent: 4 });
  assert.equal(devices(game).find((d) => d.n === 2).stale, true);
});

test('firmware is compared with the gateway, or with the reference set in the console', async () => {
  const { link, game } = setup();
  link.emitEvent(fleetStatus(MAC(1), { n: 1, fw: '1a2b3c4d' }));
  link.emitEvent(fleetStatus(MAC(2), { n: 2, fw: '0e9d8c7b' }));
  let snap = game.snapshot().fleet;
  assert.deepEqual(snap.ref, { fw: '1a2b3c4d', source: 'gateway' });
  assert.deepEqual(snap.devices.map((d) => d.fwStatus), ['current', 'outdated']);
  assert.equal((await game.act('fw-ref', { fw: '0E9D8C7B' })).ok, true);
  snap = game.snapshot().fleet;
  assert.deepEqual(snap.ref, { fw: '0e9d8c7b', source: 'console' });
  assert.deepEqual(snap.devices.map((d) => d.fwStatus), ['outdated', 'current']);
  assert.equal((await game.act('fw-ref', { fw: 'xyz' })).ok, false);
  await game.act('fw-ref', { fw: 'gateway' });
  assert.equal(game.snapshot().fleet.ref.source, 'gateway');
  link.hello = null;
  assert.deepEqual(game.snapshot().fleet.devices.map((d) => d.fwStatus), ['unknown', 'unknown']);
});

test('fleet commands go to the bridge with the fields of the protocol', async () => {
  const { link, game } = setup();
  assert.match((await game.act('update-all')).message, /neuestes Release/);
  assert.match((await game.act('update-all', { pr: 481 })).message, /PR 481/);
  assert.equal((await game.act('update-all', { pr: 'x' })).ok, false);
  assert.match((await game.act('charge', { target: 'event' })).message, /Event \(100 %\)/);
  assert.equal((await game.act('charge', { target: 'full' })).ok, false);
  await game.act('off-all');
  assert.deepEqual(link.sent.filter((c) => c.ns === 'fleet'), [
    { ns: 'fleet', cmd: 'update', source: 'release' },
    { ns: 'fleet', cmd: 'update', source: 'pr', pr: 481 },
    { ns: 'fleet', cmd: 'charge', target: 'event' },
    { ns: 'fleet', cmd: 'off' },
  ]);
});

test('members of a running session ignore fleet commands: the message says so', async () => {
  const { game } = setup();
  await game.act('session-start');
  assert.match((await game.act('charge', { target: 'store' })).message, /laufenden Sitzung ignorieren/);
});

test('ap takes a number from the fleet table or a MAC', async () => {
  const { link, game } = setup();
  link.emitEvent(fleetStatus(MAC(1), { n: 7 }));
  link.emitEvent(fleetStatus(MAC(2), { n: 3 }));
  link.emitEvent(fleetStatus(MAC(3), { n: 3 }));
  assert.match((await game.act('ap', { to: '7' })).message, /Gerät 7 \(02:b0:00:00:00:01\)/);
  assert.match((await game.act('ap', { to: MAC(9).toUpperCase() })).message, /02:b0:00:00:00:09/);
  const twice = await game.act('ap', { to: 3 });
  assert.equal(twice.ok, false);
  assert.match(twice.error, /mehrere Geräte: 02:b0:00:00:00:02, 02:b0:00:00:00:03/);
  assert.equal((await game.act('ap', { to: 44 })).ok, false);
  assert.equal((await game.act('ap', {})).ok, false);
  assert.deepEqual(link.sent.filter((c) => c.cmd === 'ap').map((c) => c.mac), [MAC(1), MAC(9)]);
});

test('off-all takes the devices on battery out of the table until they report again', async () => {
  const { link, game, clock } = setup();
  link.emitEvent(fleetStatus(MAC(1), { n: 1, usb: true }));
  link.emitEvent(fleetStatus(MAC(2), { n: 2, usb: false, state: 'active', chg: 'discharging' }));
  link.emitEvent(fleetStatus(MAC(3), { n: 3, usb: false, state: 'standby', chg: 'discharging' }));
  const r = await game.act('off-all');
  assert.match(r.message, /2 Gerät\(e\) im Akkubetrieb/);
  assert.deepEqual(devices(game).map((d) => d.n), [1]);
  // The bridge replays what it heard before the command: stays out.
  clock.advance(5000);
  link.emitEvent(fleetStatus(MAC(2), { n: 2, usb: false, age: 6 }));
  assert.deepEqual(devices(game).map((d) => d.n), [1]);
  // A device that missed the command and reports afterwards comes back.
  link.emitEvent(fleetStatus(MAC(3), { n: 3, usb: false, age: 0 }));
  assert.deepEqual(devices(game).map((d) => d.n), [1, 3]);
});

test('without fleet or config in the gateway hello nothing is sent', async () => {
  const { link, game } = setup({ spaces: { link: 0, game: 0 } });
  const r = await game.act('update-all');
  assert.equal(r.ok, false);
  assert.match(r.error, /Namensraum "fleet"/);
  assert.match((await game.act('config-get')).error, /Namensraum "config"/);
  assert.equal(link.sent.length, 0);
});

test('wifi-set sends one network and keeps the password out of messages, state and logs', async () => {
  const { link, game } = setup();
  const secret = 'Pf3rd-auf-der-Weide';
  const r = await game.act('wifi-set', { ssid: 'Halle 3', pass: secret });
  assert.equal(r.ok, true);
  assert.equal(r.message.includes(secret), false);
  assert.deepEqual(link.sent.at(-1), { ns: 'config', cmd: 'wifi', networks: [{ ssid: 'Halle 3', pass: secret }] });
  assert.equal(JSON.stringify(game.snapshot()).includes(secret), false);
  assert.equal(JSON.stringify(redact({ ...link.sent.at(-1), id: 4 })).includes(secret), false);
  assert.equal(serialize({ ...link.sent.at(-1), id: 4 }).includes(secret), true); // only the USB line carries it
  assert.equal((await game.act('wifi-set', { ssid: 'Halle 3', pass: 'kurz' })).ok, false);
  assert.equal((await game.act('wifi-set', { ssid: 'x'.repeat(33), pass: '' })).ok, false);
  assert.equal((await game.act('wifi-set', { ssid: 'Offen', pass: '' })).ok, true);
  assert.equal((await game.act('wifi-set', { ssid: 'Hex', pass: 'ab'.repeat(32) })).ok, true);
  const bad = await game.act('wifi-set', { ssid: 'Halle', pass: 'Grüße-123' });
  assert.equal(bad.ok, false);
  assert.equal(bad.error.includes('Grüße-123'), false);
  assert.equal(game.messages.some((m) => m.text.includes(secret)), false);
});

test('a too long line names only namespace and command', () => {
  assert.throws(() => serialize(cmd.configWifi([{ ssid: 'a', pass: 'geheim'.repeat(100) }])), (e) => e.message.includes('config wifi') && !e.message.includes('geheim'));
});

test('config-get reads the device on the USB cable', async () => {
  const { link, game } = setup();
  const r = await game.act('config-get');
  assert.equal(r.message, 'Gerät am USB-Kabel: Firmware 1a2b3c4d, Nummer 12, 1 WLAN-Netz(e), Ladeziel Lagern (80 %).');
  assert.deepEqual(link.sent.at(-1), { ns: 'config', cmd: 'get' });
});

test('the charging overview is a view of its own, switched by fleet-view', async () => {
  const { game } = setup();
  assert.equal(game.snapshot().view, 'start');
  await game.act('fleet-view');
  assert.equal(game.snapshot().view, 'fleet');
  await game.act('session-start');
  assert.equal(game.snapshot().view, 'fleet');
  await game.act('fleet-view', { mode: 'off' });
  assert.equal(game.snapshot().view, 'login');
  assert.equal((await game.act('fleet-view', { mode: 'maybe' })).ok, false);
});

test('factory test lines and other namespaces pass without effect', async () => {
  const { link, game } = setup();
  const before = JSON.stringify({ ...game.snapshot(), now: 0 });
  link.emitEvent({ ns: 'test', ev: 'key', key: 'l1' });
  link.emitEvent({ ns: 'test', ev: 'info', id: 1, mac: MAC(1) });
  link.emitEvent({ ns: 'fleet', ev: 'sent', id: 9 });
  link.emitEvent({ ns: 'fleet', ev: 'status', mac: 'garbage' });
  await flush();
  assert.equal(JSON.stringify({ ...game.snapshot(), now: 0 }), before);
});

test('FleetTable works on its own', () => {
  let t = 0;
  const table = new FleetTable({ now: () => t });
  assert.equal(table.onStatus(fleetStatus(MAC(1))), true);
  t = 1000;
  assert.equal(table.onStatus(fleetStatus(MAC(1), { age: 5 })), false);
  assert.equal(table.snapshot(null).devices[0].fwStatus, 'unknown');
});
