import { test } from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { GatewayLink } from '../src/link.js';
import { sleep, waitFor } from './helpers.js';

class MemTransport extends EventEmitter {
  constructor() {
    super();
    this.lines = [];
    this.writes = [];
  }

  start() {}

  async stop() {}

  write(text) {
    this.writes.push(text);
    for (const l of text.split('\n').filter(Boolean)) this.lines.push(JSON.parse(l));
  }

  feed(text) {
    this.emit('data', Buffer.from(text));
  }
}

test('says hello on open, ignores boot noise and connects on the reply', async () => {
  const t = new MemTransport();
  const link = new GatewayLink(t);
  link.start();
  t.emit('open', { path: '/dev/ttyACM0' });
  const hello = t.lines[0];
  assert.deepEqual({ ...hello, id: 0 }, { ns: 'link', cmd: 'hello', link: 0, id: 0 });
  t.feed('ESP-ROM:esp32c5\r\nrst:0x1 (POWERON)\r\n[ 123][I] boot\r\n');
  assert.equal(link.state, 'handshake');
  const events = [];
  link.on('event', (e) => events.push(e));
  t.feed(`{"ns":"link","ev":"hello","id":${hello.id},"link":0,"spaces":{"link":0,"game":0},"fw":"1a2b3c4d","mac":"02:00:00:00:00:01","proto":1,"uptimeMs":5230}\n`);
  t.feed('{"ns":"game","ev":"session","session":0,"channel":0,"fleet":0,"login":false,"active":false}\n');
  assert.equal(link.state, 'connected');
  assert.equal(link.hello.fw, '1a2b3c4d');
  assert.deepEqual(events.map((e) => e.ev), ['session']);
  await link.stop();
});

test('hello is repeated until the gateway answers', async () => {
  const t = new MemTransport();
  const link = new GatewayLink(t);
  link.start();
  t.emit('open', { path: 'x' });
  await waitFor(() => t.lines.length >= 2, { timeoutMs: 3000, what: 'second hello' });
  assert.ok(t.lines.every((l) => l.cmd === 'hello'));
  assert.notEqual(t.lines[0].id, t.lines[1].id);
  await link.stop();
});

test('replies resolve by id, error replies reject with the code', async () => {
  const t = new MemTransport();
  const link = new GatewayLink(t);
  link.start();
  t.emit('open', { path: 'x' });
  t.feed(`{"ns":"link","ev":"hello","id":${t.lines[0].id},"link":0,"spaces":{"link":0,"game":0}}\n`);
  const p1 = link.request({ ns: 'game', cmd: 'round', round: 1, type: 'idle' });
  const p2 = link.request({ ns: 'game', cmd: 'admit', mac: '02:00:00:00:00:11', n: 17 });
  const [r1, r2] = t.lines.slice(1);
  t.feed(`{"ns":"link","ev":"error","id":${r2.id},"code":"admit_timeout","refNs":"game","refCmd":"admit"}\n{"ns":"game","ev":"state","id":${r1.id},"round":1,"seq":1,"type":"idle","phase":null,"changed":true}\n`);
  assert.equal((await p1).ev, 'state');
  await assert.rejects(p2, (e) => e.code === 'admit_timeout');
  await link.stop();
});

test('a command without reply is sent again, then fails with timeout', async () => {
  const t = new MemTransport();
  const link = new GatewayLink(t);
  link.start();
  t.emit('open', { path: 'x' });
  t.feed(`{"ns":"link","ev":"hello","id":${t.lines[0].id},"link":0,"spaces":{"link":0,"game":0}}\n`);
  const p = link.request({ ns: 'game', cmd: 'session', session: 1, channel: 6, fleet: 0, login: true }, { timeoutMs: 50, retries: 1 });
  await assert.rejects(p, (e) => e.code === 'timeout');
  assert.equal(t.lines.filter((l) => l.cmd === 'session').length, 2);
  await link.stop();
});

test('ping after 2 s without other lines, pong fills the counters', async () => {
  const t = new MemTransport();
  const link = new GatewayLink(t);
  link.start();
  t.emit('open', { path: 'x' });
  t.feed(`{"ns":"link","ev":"hello","id":${t.lines[0].id},"link":0,"spaces":{"link":0,"game":0}}\n`);
  const ping = await waitFor(() => t.lines.find((l) => l.cmd === 'ping'), { timeoutMs: 3000, what: 'ping' });
  t.feed(`{"ns":"link","ev":"pong","id":${ping.id},"uptimeMs":65230,"rxFrames":1840,"txFrames":402,"rxOverflow":1,"dropped":2}\n`);
  assert.deepEqual(link.counters, { uptimeMs: 65230, rxFrames: 1840, txFrames: 402, rxOverflow: 1, dropped: 2 });
  await link.stop();
});

test('closing the port fails open requests and the next open says hello again', async () => {
  const t = new MemTransport();
  const link = new GatewayLink(t);
  link.start();
  t.emit('open', { path: 'x' });
  t.feed(`{"ns":"link","ev":"hello","id":${t.lines[0].id},"link":0,"spaces":{"link":0,"game":0}}\n`);
  const p = link.request({ ns: 'game', cmd: 'end' });
  t.emit('close');
  await assert.rejects(p, (e) => e.code === 'offline');
  assert.equal(link.state, 'closed');
  await assert.rejects(link.request({ ns: 'game', cmd: 'end' }), (e) => e.code === 'offline');
  t.emit('open', { path: 'x' });
  await sleep(5);
  assert.equal(t.lines.at(-1).cmd, 'hello');
  await link.stop();
});

test('several commands go out in one write, each reply resolves its own promise', async () => {
  const t = new MemTransport();
  const link = new GatewayLink(t);
  link.start();
  t.emit('open', { path: 'x' });
  t.feed(`{"ns":"link","ev":"hello","id":${t.lines[0].id},"link":0,"spaces":{"link":0,"game":0}}\n`);
  const before = t.writes.length;
  const [ps, pr] = link.requestAll([
    { ns: 'game', cmd: 'session', session: 4711, channel: 6, fleet: 0, login: false },
    { ns: 'game', cmd: 'round', round: 23417, type: 'idle' },
  ]);
  assert.equal(t.writes.length, before + 1);
  assert.equal(t.writes.at(-1).split('\n').filter(Boolean).length, 2);
  const [s, r] = t.lines.slice(-2);
  assert.deepEqual([s.cmd, r.cmd], ['session', 'round']);
  t.feed(`{"ns":"game","ev":"state","id":${r.id},"round":23417,"seq":1,"type":"idle","phase":null,"changed":true}\n{"ns":"game","ev":"session","id":${s.id},"session":4711,"channel":6,"fleet":0,"login":false,"active":true}\n`);
  assert.equal((await ps).ev, 'session');
  assert.equal((await pr).ev, 'state');
  await link.stop();
});
