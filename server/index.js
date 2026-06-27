import http from 'http';
import fs from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import express from 'express';
import { WebSocketServer } from 'ws';
import { authRouter, requireAuth } from './auth.js';

const DATA_DIR = join(dirname(fileURLToPath(import.meta.url)), '..', 'data');

const MOCK_CONFIG = {
  deviceName: 'Mock Device',
  otaPort: 3232,
  otaEnabled: true,
  groupId: 0,
  ledType: 0,
  dataPin: 13,
  clockPin: 14,
  logLevel: 1,
  sceneSyncEnabled: true,
  checkUpdateOnStartup: false,
  mqttHost: 'mqtt.local',
  mqttPort: 1883,
  mqttUser: 'lights',
  version: 'mock',
  mac: '11:22:33:44:55:66',
  githubRepo: 'Variour/batterylight',
  groups: [
    { id: 0, name: 'Default',     exists: true, mode: 0, sceneId: '',          pattern: 0, r: 255, g: 200, b: 80,  brightness: 200, speed: 1, syncEnabled: true,  transitionEnabled: false, transitionTime: 1.0, proximityScale: 1.0 },
    { id: 1, name: 'Scene Group', exists: true, mode: 1, sceneId: 'a1b2c3d4e5f6470789abcdef', pattern: 0, r: 255, g: 100, b: 50, brightness: 180, speed: 1, syncEnabled: false, transitionEnabled: true,  transitionTime: 0.5, proximityScale: 1.0 },
  ],
};

const MOCK_SELF  = { name: 'Mock Device',   mac: '11:22:33:44:55:66', groupId: 0, online: true,  sceneSyncEnabled: true,  wifiConnected: true,  version: 'mock',         fwState: 'idle'  };
const MOCK_PEERS = [
  { name: 'Mock Light 2', mac: '22:33:44:55:66:77', groupId: 0, online: true,  rssi: -65, sceneSyncEnabled: true,  wifiConnected: true,  version: 'mock',         fwState: 'error' },
  { name: 'Mock Light 3', mac: '33:44:55:66:77:88', groupId: 1, online: false, rssi: -80, sceneSyncEnabled: false, wifiConnected: false, version: '2026.01.01.0', fwState: 'idle'  },
];

const wifiNetworks = [
  { ssid: 'HomeNetwork', password: 'secret1' },
  { ssid: 'WorkWifi',    password: 'secret2' },
];
let wifiConnected = 'HomeNetwork';

const scenes = new Map();

// Pre-populate scenes from all static JSON files in data/sc/
try {
  const scenesDir = join(DATA_DIR, 'sc');
  for (const f of fs.readdirSync(scenesDir).filter(f => f.endsWith('.json'))) {
    const raw = JSON.parse(fs.readFileSync(join(scenesDir, f), 'utf8'));
    scenes.set(raw.id, raw);
  }
} catch (_) {}

const mockUpdate = {
  currentVersion: 'mock',
  latestVersion: '',
  hasUpdate: false,
  progress: 0,
  state: 'checking',  // simulate boot-time check in progress
  error: null,
};

// Resolve the simulated boot check after 3 s — exercises the poll-while-checking
// path and causes the update badge to appear without any user action.
setTimeout(() => {
  mockUpdate.state = 'idle';
  mockUpdate.latestVersion = '9999.99.99.0';
  mockUpdate.hasUpdate = true;
}, 3000);

const app = express();
app.set('trust proxy', true);
app.use(express.json());
app.use('/auth', authRouter);
app.use(requireAuth);
app.use(express.static(DATA_DIR));

app.get('/api/config', (_req, res) => res.json(MOCK_CONFIG));
app.post('/api/config', (_req, res) => res.json({ ok: true }));

app.get('/api/wifi', (_req, res) => res.json({
  connected: wifiConnected,
  networks: wifiNetworks.map(n => n.ssid),
}));
app.post('/api/wifi/add', (req, res) => {
  const { ssid, password } = req.body;
  if (!ssid) return res.status(400).json({ error: 'ssid required' });
  const existing = wifiNetworks.find(n => n.ssid === ssid);
  if (existing) { existing.password = password || ''; return res.json({ ok: true }); }
  if (wifiNetworks.length >= 5) return res.status(409).json({ error: 'network list full' });
  wifiNetworks.push({ ssid, password: password || '' });
  res.json({ ok: true });
});
app.post('/api/wifi/delete', (req, res) => {
  const { ssid } = req.body;
  if (!ssid) return res.status(400).json({ error: 'ssid required' });
  const idx = wifiNetworks.findIndex(n => n.ssid === ssid);
  if (idx !== -1) wifiNetworks.splice(idx, 1);
  if (wifiConnected === ssid) wifiConnected = null;
  res.json({ ok: true });
});

app.get('/api/peers', (_req, res) => res.json({ self: MOCK_SELF, peers: MOCK_PEERS }));
app.post('/api/peers/setgroup',     (_req, res) => res.json({ ok: true }));
app.post('/api/peers/setscenesync', (_req, res) => res.json({ ok: true }));
app.post('/api/peers/pushconfig',      (_req, res) => res.json({ ok: true }));
app.post('/api/peers/triggerupdate',   (_req, res) => res.json({ ok: true }));

app.get('/api/scenes', (_req, res) => {
  const list = [...scenes.values()].map(({ id, name, w, h, fc }) => ({ id, name, w, h, fc }));
  res.json({ scenes: list });
});

app.get('/api/scenes/get', (req, res) => {
  const scene = scenes.get(req.query.id);
  if (!scene) return res.status(404).json({ error: 'not found' });
  res.json(scene);
});

app.post('/api/scenes/create', (req, res) => {
  const name = String(req.body.name || 'Unnamed');
  const w = Math.max(1, Math.min(200, parseInt(req.body.w) || 20));
  const h = Math.max(1, Math.min(200, parseInt(req.body.h) || 10));
  const id = `mock-${Date.now().toString(36)}`;
  const defaultFrame = Array.from({ length: w * h }, () => '000000');
  scenes.set(id, { id, name, w, h, fc: 1, frames: [defaultFrame] });
  res.json({ ok: true, id });
});

app.post('/api/scenes/save', (req, res) => {
  const { id } = req.body;
  if (!id) return res.status(400).json({ error: 'missing id' });
  const fc = Array.isArray(req.body.frames) ? req.body.frames.length : (parseInt(req.body.fc) || 0);
  scenes.set(id, { ...req.body, fc });
  res.json({ ok: true });
});

app.post('/api/scenes/delete', (req, res) => {
  if (req.body.id) scenes.delete(req.body.id);
  res.json({ ok: true });
});

app.get('/api/scenes/sync/conflicts', (_req, res) => res.json({ conflicts: [], peerScenes: [] }));
app.post('/api/scenes/sync/resolve',  (_req, res) => res.json({ ok: true }));

app.post('/api/groups/create',  (_req, res) => res.json({ ok: true }));
app.post('/api/groups/update',  (_req, res) => res.json({ ok: true }));
app.post('/api/groups/delete',  (_req, res) => res.json({ ok: true }));
app.post('/api/reset',          (_req, res) => res.json({ ok: true }));
app.post('/api/mesh/search',    (_req, res) => res.json({ ok: true }));

app.get('/api/update/status', (_req, res) => {
  const out = {
    currentVersion: mockUpdate.currentVersion,
    latestVersion:  mockUpdate.latestVersion,
    hasUpdate:      mockUpdate.hasUpdate,
    progress:       mockUpdate.progress,
    state:          mockUpdate.state,
  };
  if (mockUpdate.error) out.error = mockUpdate.error;
  res.json(out);
});

app.post('/api/update/check', (_req, res) => {
  mockUpdate.state = 'checking';
  mockUpdate.error = null;
  res.json({ ok: true });
  setTimeout(() => {
    mockUpdate.state = 'idle';
    mockUpdate.latestVersion = '1.2.0';
    mockUpdate.hasUpdate = true;
  }, 2000);
});

app.post('/api/update/apply', (_req, res) => {
  if (!mockUpdate.hasUpdate) {
    return res.status(400).json({ error: 'no update available' });
  }
  mockUpdate.state = 'downloading';
  mockUpdate.progress = 0;
  res.json({ ok: true });
  let p = 0;
  const iv = setInterval(() => {
    p += 10;
    mockUpdate.progress = p;
    if (p >= 100) {
      clearInterval(iv);
      mockUpdate.state = 'done';
    }
  }, 500);
});

app.post('/api/update/trigger', (_req, res) => {
  res.json({ ok: true });
  if (mockUpdate.hasUpdate) {
    // Simulate the apply flow
    mockUpdate.state = 'downloading';
    mockUpdate.progress = 0;
    let p = 0;
    const iv = setInterval(() => {
      p += 10;
      mockUpdate.progress = p;
      if (p >= 100) { clearInterval(iv); mockUpdate.state = 'done'; }
    }, 500);
  } else {
    // Simulate check then apply
    mockUpdate.state = 'checking';
    setTimeout(() => {
      mockUpdate.state = 'idle';
      mockUpdate.latestVersion = '9999.99.99.0';
      mockUpdate.hasUpdate = true;
    }, 2000);
  }
});

app.get('/*path', (_req, res) => res.sendFile(join(DATA_DIR, 'index.html')));

const server = http.createServer(app);

const wss = new WebSocketServer({ server, path: '/ws' });
wss.on('connection', ws => {
  const send = data => ws.send(JSON.stringify(data));
  send({ t: 'log', l: 'I', m: 'Mock server connected' });
  send({ t: 'log', l: 'I', m: 'This is a development mock — no hardware attached' });
  send({ t: 'peers', self: MOCK_SELF, peers: MOCK_PEERS });
  send({ t: 'groups', list: MOCK_CONFIG.groups });
});

const PORT = process.env.PORT || 8080;
server.listen(PORT, () => console.log(`Battery Light mock server: http://localhost:${PORT}`));
