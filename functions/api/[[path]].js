const MOCK_CONFIG = {
  deviceName: 'Mock Device',
  wifiSsid: '',
  otaPort: 3232,
  ledType: 0,
  logLevel: 1,
  mqttHost: '',
  mqttPort: 1883,
  mqttUser: '',
  version: 'mock',
  mac: '11:22:33:44:55:66',
  groups: [
    { id: 0, name: 'Default', mode: 0, pattern: 0, r: 255, g: 200, b: 80, brightness: 200, speed: 1, syncEnabled: true },
  ],
};

const MOCK_PEERS = {
  self: { name: 'Mock Device', mac: '11:22:33:44:55:66', groupId: 0, online: true },
  peers: [
    { name: 'Mock Light 2', mac: '22:33:44:55:66:77', groupId: 0, online: true, rssi: -65 },
  ],
};

function json(data, status = 200) {
  return Response.json(data, { status });
}

async function listScenes(kv) {
  const list = await kv.list({ prefix: 'scene:' });
  return list.keys.map(k => ({ id: k.name.slice(6), ...k.metadata }));
}

export async function onRequest(ctx) {
  const { request, env } = ctx;
  const url = new URL(request.url);
  const path = url.pathname;
  const method = request.method;
  const kv = env.SCENES;

  let body = {};
  if (method === 'POST') {
    try { body = await request.json(); } catch {}
  }

  if (path === '/api/config') {
    if (method === 'GET') return json(MOCK_CONFIG);
    if (method === 'POST') return json({ ok: true });
  }

  if (path === '/api/peers' && method === 'GET') {
    return json(MOCK_PEERS);
  }

  if (path === '/api/scenes' && method === 'GET') {
    const scenes = kv ? await listScenes(kv) : [];
    return json({ scenes });
  }

  if (path === '/api/scenes/get' && method === 'GET') {
    const id = url.searchParams.get('id');
    if (!id) return json({ error: 'missing id' }, 400);
    if (!kv) return json({ error: 'storage not configured' }, 500);
    const scene = await kv.get('scene:' + id, { type: 'json' });
    if (!scene) return json({ error: 'not found' }, 404);
    return json(scene);
  }

  if (path === '/api/scenes/create' && method === 'POST') {
    if (!kv) return json({ error: 'storage not configured' }, 500);
    const name = String(body.name || 'Unnamed');
    const w = Math.max(1, Math.min(200, parseInt(body.w) || 20));
    const h = Math.max(1, Math.min(200, parseInt(body.h) || 10));
    const id = 'mock-' + crypto.randomUUID().slice(0, 8);
    const scene = { id, name, w, h, fc: 0, frames: [] };
    await kv.put('scene:' + id, JSON.stringify(scene), { metadata: { name, w, h, fc: 0 } });
    return json({ ok: true, id });
  }

  if (path === '/api/scenes/save' && method === 'POST') {
    if (!kv) return json({ error: 'storage not configured' }, 500);
    const id = body.id;
    if (!id) return json({ error: 'missing id' }, 400);
    const fc = Array.isArray(body.frames) ? body.frames.length : (parseInt(body.fc) || 0);
    const scene = { ...body, fc };
    await kv.put('scene:' + id, JSON.stringify(scene), {
      metadata: { name: String(body.name || 'Unnamed'), w: body.w || 0, h: body.h || 0, fc },
    });
    return json({ ok: true });
  }

  if (path === '/api/scenes/delete' && method === 'POST') {
    if (kv && body.id) await kv.delete('scene:' + body.id);
    return json({ ok: true });
  }

  if (
    ['/api/groups/create', '/api/groups/update', '/api/groups/delete',
     '/api/peers/setgroup', '/api/reset'].includes(path) && method === 'POST'
  ) {
    return json({ ok: true });
  }

  return json({ error: 'not found' }, 404);
}
