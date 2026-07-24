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
  test('strips write-only secrets before merging, reports rebooting for deviceName', async () => {
    const res = await fetch(`${baseUrl}/api/config`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ deviceName: 'Renamed', mqttPassword: 'secret', githubToken: 'ghp_x' }),
    });
    assert.equal(res.status, 200);
    assert.deepEqual(await res.json(), { ok: true, rebooting: true });

    const config = await (await fetch(`${baseUrl}/api/config`)).json();
    assert.equal(config.deviceName, 'Renamed');
    assert.equal(config.mqttPassword, undefined);
    assert.equal(config.githubToken, undefined);
  });

  test('does not report rebooting when only reboot-free fields change', async () => {
    const res = await fetch(`${baseUrl}/api/config`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ checkUpdateOnStartup: true, mqttHost: 'broker.local' }),
    });
    assert.equal(res.status, 200);
    assert.deepEqual(await res.json(), { ok: true, rebooting: false });

    const config = await (await fetch(`${baseUrl}/api/config`)).json();
    assert.equal(config.checkUpdateOnStartup, true);
    assert.equal(config.mqttHost, 'broker.local');
  });

  test('reports rebooting: false when a request repeats the current deviceName unchanged', async () => {
    const current = await (await fetch(`${baseUrl}/api/config`)).json();
    const res = await fetch(`${baseUrl}/api/config`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ deviceName: current.deviceName, logLevel: current.logLevel }),
    });
    assert.equal(res.status, 200);
    assert.deepEqual(await res.json(), { ok: true, rebooting: false });
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

describe('POST /api/lights/add', () => {
  test('rejects a dataPin that collides with another light', async () => {
    const res = await fetch(`${baseUrl}/api/lights/add`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name: 'Dup', ledType: 0, dataPin: 13, clockPin: 40 }),  // 13 = Living room's dataPin
    });
    assert.equal(res.status, 400);
    assert.equal((await res.json()).error, 'pin already in use');
  });

  test('two WS2812B lights left at the same default clock pin do not collide (clockPin is unused for WS2812B)', async () => {
    const res = await fetch(`${baseUrl}/api/lights/add`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name: 'Hallway', ledType: 0, dataPin: 41, clockPin: 14 }),  // 14 = Living room's (unused) clockPin
    });
    assert.equal(res.status, 200);
    const { index } = await res.json();
    await fetch(`${baseUrl}/api/lights/delete`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ index }),
    });
  });

  test('rejects a WS2801 clockPin that collides with another WS2801 light', async () => {
    const res = await fetch(`${baseUrl}/api/lights/add`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name: 'Dup', ledType: 1, dataPin: 42, clockPin: 26 }),  // 26 = Bedroom's clockPin
    });
    assert.equal(res.status, 400);
    assert.equal((await res.json()).error, 'pin already in use');
  });

  test('rejects a WS2801 light whose dataPin and clockPin are the same', async () => {
    const res = await fetch(`${baseUrl}/api/lights/add`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name: 'Bad', ledType: 1, dataPin: 43, clockPin: 43 }),
    });
    assert.equal(res.status, 400);
    assert.equal((await res.json()).error, 'duplicate pin within light config');
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

  test('rejects a dataPin update that collides with another light, leaving it unchanged', async () => {
    const res = await fetch(`${baseUrl}/api/lights/update`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ index: 2, dataPin: 13 }),  // 13 = Living room's dataPin; 2 = Patio
    });
    assert.equal(res.status, 400);
    assert.equal((await res.json()).error, 'pin already in use');
    const { lights } = await (await fetch(`${baseUrl}/api/lights`)).json();
    assert.equal(lights.find(l => l.index === 2).dataPin, 27);
  });
});

describe('POST /api/lights/override', () => {
  const postJson = (path, body) => fetch(`${baseUrl}${path}`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  });
  const overridden = async index =>
    (await (await fetch(`${baseUrl}/api/lights`)).json()).lights.find(l => l.index === index).overridden;

  test('marks the seeded Patio override in /api/lights and self.lights', async () => {
    assert.equal(await overridden(2), true);
    assert.equal(await overridden(0), false);
    const { self } = await (await fetch(`${baseUrl}/api/peers`)).json();
    assert.equal(self.lights.find(l => l.index === 2).overridden, true);
  });

  test('returns 404 for an unknown light index', async () => {
    const res = await postJson('/api/lights/override', { index: 99, r: 0, g: 255, b: 0 });
    assert.equal(res.status, 404);
  });

  test('sets an override on a light', async () => {
    const res = await postJson('/api/lights/override', { index: 0, r: 0, g: 255, b: 0 });
    assert.equal(res.status, 200);
    assert.equal(await overridden(0), true);
  });

  test('a real group change displaces the override, a name-only edit does not', async () => {
    await postJson('/api/lights/override', { index: 0, r: 0, g: 255, b: 0 });
    let res = await postJson('/api/groups/update', { id: 0, name: 'Default' });
    assert.equal(res.status, 200);
    assert.equal(await overridden(0), true);
    res = await postJson('/api/groups/update', { id: 0, brightness: 123 });
    assert.equal(res.status, 200);
    assert.equal(await overridden(0), false);
  });

  test('reassigning the light to another group displaces the override', async () => {
    await postJson('/api/lights/override', { index: 0, r: 0, g: 255, b: 0 });
    await postJson('/api/lights/update', { index: 0, groupId: 1 });
    assert.equal(await overridden(0), false);
    await postJson('/api/lights/update', { index: 0, groupId: 0 });
  });

  test('a durationMs override expires on its own', async () => {
    await postJson('/api/lights/override', { index: 0, r: 0, g: 255, b: 0, durationMs: 40 });
    assert.equal(await overridden(0), true);
    await new Promise(resolve => setTimeout(resolve, 120));
    assert.equal(await overridden(0), false);
  });
});

describe('POST /api/lights/override/clear', () => {
  test('returns 404 for an unknown light index', async () => {
    const res = await fetch(`${baseUrl}/api/lights/override/clear`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ index: 99 }),
    });
    assert.equal(res.status, 404);
  });

  test('clears the seeded override — the light follows its group again', async () => {
    const res = await fetch(`${baseUrl}/api/lights/override/clear`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ index: 2 }),
    });
    assert.equal(res.status, 200);
    const { lights } = await (await fetch(`${baseUrl}/api/lights`)).json();
    assert.equal(lights.find(l => l.index === 2).overridden, false);
  });
});

describe('GET /api/sounds', () => {
  test('returns the configured sound outputs', async () => {
    const res = await fetch(`${baseUrl}/api/sounds`);
    assert.equal(res.status, 200);
    const body = await res.json();
    assert.ok(Array.isArray(body.sounds));
    assert.ok(body.sounds.some(s => s.name === 'Speaker'));
  });
});

describe('POST /api/sounds/add', () => {
  test('rejects once the (single) sound slot is already taken', async () => {
    const res = await fetch(`${baseUrl}/api/sounds/add`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ i2cSdaPin: 1, i2cSclPin: 2, i2sBclkPin: 3, i2sWsPin: 4, i2sDoutPin: 6 }),
    });
    assert.equal(res.status, 400);
    assert.equal((await res.json()).error, 'sound limit reached');
  });
});

describe('POST /api/sounds/update', () => {
  test('returns 404 for an unknown sound index', async () => {
    const res = await fetch(`${baseUrl}/api/sounds/update`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ index: 99, name: 'Nope' }),
    });
    assert.equal(res.status, 404);
  });

  test('rejects a pin that collides with a configured light', async () => {
    const res = await fetch(`${baseUrl}/api/sounds/update`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ index: 0, i2sDoutPin: 13 }),  // 13 = Living room light's dataPin
    });
    assert.equal(res.status, 400);
    assert.equal((await res.json()).error, 'pin already in use');
  });

  test('renaming does not require the other hardware fields', async () => {
    const res = await fetch(`${baseUrl}/api/sounds/update`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ index: 0, name: 'Renamed speaker' }),
    });
    assert.equal(res.status, 200);
    const { sounds } = await (await fetch(`${baseUrl}/api/sounds`)).json();
    assert.equal(sounds.find(s => s.index === 0).name, 'Renamed speaker');
  });

  test('an expander PA-enable pin index does not collide with a same-numbered GPIO', async () => {
    // The mock sound starts with paViaExpander=true; pin 4 is a native GPIO
    // used by the "Wall switch" button, but as an expander pin index it's a
    // different address space and must be accepted.
    const res = await fetch(`${baseUrl}/api/sounds/update`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ index: 0, paEnablePin: 4 }),
    });
    assert.equal(res.status, 200);
  });

  test('the same pin number is rejected once switched to a direct GPIO', async () => {
    const res = await fetch(`${baseUrl}/api/sounds/update`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ index: 0, paViaExpander: false, paEnablePin: 4 }),
    });
    assert.equal(res.status, 400);
    assert.equal((await res.json()).error, 'pin already in use');
  });
});

describe('GET /api/storage', () => {
  test('returns hardware support, card presence, and the .wav file list', async () => {
    const res = await fetch(`${baseUrl}/api/storage`);
    assert.equal(res.status, 200);
    const body = await res.json();
    assert.equal(body.hwSupported, true);
    assert.equal(body.present, true);
    assert.ok(Array.isArray(body.files));
    assert.ok(body.files.some(f => f.name === 'doorbell.wav'));
  });

  // The card's root can carry pre-existing, non-.wav files (a prior
  // recording, OS metadata from formatting on a computer, ...) — those must
  // never be listed, since upload/delete would 400 "invalid filename" on
  // them (this is the bug this test guards against — see WebServer.h::_getStorage).
  test('excludes non-.wav files from the list but still counts them in usedBytes', async () => {
    const res = await fetch(`${baseUrl}/api/storage`);
    const body = await res.json();
    assert.ok(!body.files.some(f => f.name === 'IMG_0001.JPG'));
    assert.ok(body.usedBytes > body.files.reduce((s, f) => s + f.size, 0));
  });
});

describe('POST /api/storage/upload', () => {
  test('rejects a non-.wav filename', async () => {
    const res = await fetch(`${baseUrl}/api/storage/upload?name=song.mp3`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/octet-stream' },
      body: Buffer.from('data'),
    });
    assert.equal(res.status, 400);
    assert.equal((await res.json()).error, 'invalid filename');
  });

  test('rejects a filename with a path separator', async () => {
    const res = await fetch(`${baseUrl}/api/storage/upload?name=${encodeURIComponent('../evil.wav')}`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/octet-stream' },
      body: Buffer.from('data'),
    });
    assert.equal(res.status, 400);
    assert.equal((await res.json()).error, 'invalid filename');
  });

  test('accepts a valid .wav upload and it shows up in the file list', async () => {
    const payload = Buffer.from('RIFF....WAVEfmt ');
    const res = await fetch(`${baseUrl}/api/storage/upload?name=greeting.wav`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/octet-stream' },
      body: payload,
    });
    assert.equal(res.status, 200);
    assert.deepEqual(await res.json(), { ok: true });

    const { files } = await (await fetch(`${baseUrl}/api/storage`)).json();
    const uploaded = files.find(f => f.name === 'greeting.wav');
    assert.ok(uploaded);
    assert.equal(uploaded.size, payload.length);
  });
});

describe('POST /api/storage/delete', () => {
  test('returns 404 for an unknown file', async () => {
    const res = await fetch(`${baseUrl}/api/storage/delete`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name: 'nope.wav' }),
    });
    assert.equal(res.status, 404);
  });

  test('deletes an existing file', async () => {
    const res = await fetch(`${baseUrl}/api/storage/delete`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name: 'alarm.wav' }),
    });
    assert.equal(res.status, 200);
    const { files } = await (await fetch(`${baseUrl}/api/storage`)).json();
    assert.ok(!files.some(f => f.name === 'alarm.wav'));
  });
});
