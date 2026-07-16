import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';

process.env.DEV_NO_AUTH = 'true';
process.env.PORT = '0';

const { server } = await import('./index.js');

let baseUrl;

before(() => new Promise(resolve => {
  if (server.listening) {
    baseUrl = `http://localhost:${server.address().port}`;
    return resolve();
  }
  server.once('listening', () => {
    baseUrl = `http://localhost:${server.address().port}`;
    resolve();
  });
}));

after(() => new Promise(resolve => server.close(resolve)));

describe('GET /api/config', () => {
  test('returns the mock device config', async () => {
    const res = await fetch(`${baseUrl}/api/config`);
    assert.equal(res.status, 200);
    const body = await res.json();
    assert.equal(body.deviceName, 'Mock Device');
    assert.equal(body.mac, '11:22:33:44:55:66');
  });
});

describe('POST /api/config', () => {
  test('strips write-only secrets before merging', async () => {
    const res = await fetch(`${baseUrl}/api/config`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ deviceName: 'Renamed', mqttPassword: 'secret', githubToken: 'ghp_x' }),
    });
    assert.equal(res.status, 200);
    assert.deepEqual(await res.json(), { ok: true });

    const config = await (await fetch(`${baseUrl}/api/config`)).json();
    assert.equal(config.deviceName, 'Renamed');
    assert.equal(config.mqttPassword, undefined);
    assert.equal(config.githubToken, undefined);
  });
});

describe('GET /api/wifi', () => {
  test('lists known networks', async () => {
    const res = await fetch(`${baseUrl}/api/wifi`);
    assert.equal(res.status, 200);
    const body = await res.json();
    assert.ok(body.networks.includes('HomeNetwork'));
  });
});

describe('POST /api/wifi/add', () => {
  test('rejects a request missing ssid', async () => {
    const res = await fetch(`${baseUrl}/api/wifi/add`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ password: 'x' }),
    });
    assert.equal(res.status, 400);
  });

  test('adds a new network', async () => {
    const res = await fetch(`${baseUrl}/api/wifi/add`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ssid: 'NewNetwork', password: 'pw' }),
    });
    assert.equal(res.status, 200);
    const { networks } = await (await fetch(`${baseUrl}/api/wifi`)).json();
    assert.ok(networks.includes('NewNetwork'));
  });
});

describe('GET /api/lights', () => {
  test('returns the configured lights', async () => {
    const res = await fetch(`${baseUrl}/api/lights`);
    assert.equal(res.status, 200);
    const body = await res.json();
    assert.ok(Array.isArray(body.lights));
    assert.ok(body.lights.some(l => l.name === 'Living room'));
  });
});

describe('POST /api/lights/update', () => {
  test('returns 404 for an unknown light index', async () => {
    const res = await fetch(`${baseUrl}/api/lights/update`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ index: 99, name: 'Nope' }),
    });
    assert.equal(res.status, 404);
  });
});
