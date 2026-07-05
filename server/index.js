import http from 'http';
import fs from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import express from 'express';
import { WebSocketServer } from 'ws';
import { authRouter, requireAuth } from './auth.js';

const DATA_DIR = join(dirname(fileURLToPath(import.meta.url)), '..', 'data');

const mockLights = [
  { index: 0, name: 'Living room', ledType: 0, dataPin: 13, clockPin: 14, width: 1,  height: 1, matrixStart: 0, matrixDir: 0, matrixSerpentine: false, wrapWidth: false, wrapHeight: false, groupId: 0 },
  { index: 1, name: 'Bedroom',     ledType: 1, dataPin: 25, clockPin: 26, width: 8,  height: 8, matrixStart: 2, matrixDir: 1, matrixSerpentine: false, wrapWidth: false, wrapHeight: false, groupId: 1 },
  { index: 2, name: 'Patio',       ledType: 0, dataPin: 27, clockPin: 32, width: 12, height: 1, matrixStart: 0, matrixDir: 0, matrixSerpentine: false, wrapWidth: true,  wrapHeight: false, groupId: 2 },
];

// The real device derives self.lights in /api/peers straight from the live
// hardware config on every request (see WebServer.h::_buildPeersJson), so it
// can never drift. Mirror that here instead of keeping a second static copy.
function mockSelfLights() {
  return mockLights.map(({ index, name, groupId, ledType, width, height, wrapWidth }) =>
    ({ index, name, groupId, ledType, width, height, wrapWidth }));
}

const MOCK_CONFIG = {
  deviceName: 'Mock Device',
  otaPort: 3232,
  otaEnabled: true,
  logLevel: 1,
  sceneSyncEnabled: true,
  checkUpdateOnStartup: false,
  mqttHost: 'mqtt.local',
  mqttPort: 1883,
  mqttUser: 'lights',
  version: 'mock',
  mac: '11:22:33:44:55:66',
  githubRepo: 'Variour/batterylight',
  lights: mockLights,
  groups: [
    { id: 0, name: 'Default',     exists: true, mode: 0, sceneId: '',          pattern: 0, r: 255, g: 200, b: 80,  brightness: 200, speed: 1, syncEnabled: true,  transitionEnabled: false, sceneUniformColor: false, transitionTime: 2.0, frameDuration: 1.0, proximityScale: 1.0, morphEnabled: false, gradientStopCount: 0, text: '', textAnimation: 0 },
    { id: 1, name: 'Scene Group', exists: true, mode: 1, sceneId: '0002ee38f7ce6ab7acd6a859', pattern: 0, r: 255, g: 100, b: 50, brightness: 180, speed: 1, syncEnabled: false, transitionEnabled: false, sceneUniformColor: false, transitionTime: 2.0, frameDuration: 1.0, proximityScale: 1.0, morphEnabled: false, gradientStopCount: 0, text: '', textAnimation: 0 },
    { id: 2, name: 'Gradient Group', exists: true, mode: 3, sceneId: '0002ee38f7ce6ab7acd6a859', pattern: 0, r: 255, g: 255, b: 255, brightness: 220, speed: 1, syncEnabled: true, transitionEnabled: false, sceneUniformColor: false, transitionTime: 2.0, frameDuration: 1.0, proximityScale: 1.0, morphEnabled: true, gradientStopCount: 5, text: '', textAnimation: 0 },
  ],
};

const MOCK_SELF  = { name: 'Mock Device',   mac: '11:22:33:44:55:66', online: true,  sceneSyncEnabled: true,  wifiConnected: true,  version: '2026.06.27.0', fwState: 'checking' };
const MOCK_PEERS = [
  { name: 'Mock Light 2', mac: '22:33:44:55:66:77', lights: [{ index: 0, name: 'Kitchen', groupId: 0 }], online: true,  rssi: -65, sceneSyncEnabled: true,  wifiConnected: true,  version: '2026.01.01.0', fwState: 'idle'  },
  { name: 'Mock Light 3', mac: '33:44:55:66:77:88', lights: [{ index: 0, name: 'Hallway', groupId: 1 }, { index: 1, name: 'Closet', groupId: 0 }], online: true,  rssi: -80, sceneSyncEnabled: false, wifiConnected: false, version: '2026.01.01.0', fwState: 'idle'  },
  { name: 'Mock Light 4', mac: '44:55:66:77:88:99', lights: [{ index: 0, name: '', groupId: 0 }], online: true,  rssi: -55, sceneSyncEnabled: true,  wifiConnected: true,  version: '2026.06.27.0', fwState: 'idle'  },
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

let _rebooting = false;

function selfWithLights() {
  return { ...MOCK_SELF, lights: mockSelfLights() };
}

// Broadcast current self+peers state to all connected WS clients.
function broadcastPeers() {
  const msg = JSON.stringify({ t: 'peers', self: selfWithLights(), peers: MOCK_PEERS });
  wss.clients.forEach(c => { if (c.readyState === 1) c.send(msg); });
}

// Simulate a device reboot: drop WS connections, block API for ~4 s, then
// come back online with the new version.
//
// Delayed 1.5 s before going offline so the browser's next 1-s poll can still
// see state='done' and set _otaRebooting before requests start returning 503.
// WS reconnects are also rejected while rebooting (see wss.on('connection')).
function simulateReboot(newVersion) {
  MOCK_SELF.fwState = 'done';
  broadcastPeers();
  setTimeout(() => {
    _rebooting = true;
    wss.clients.forEach(c => c.terminate());
    setTimeout(() => {
      MOCK_CONFIG.version       = newVersion;
      MOCK_SELF.version         = newVersion;
      MOCK_SELF.fwState         = 'idle';
      mockUpdate.currentVersion = newVersion;
      mockUpdate.latestVersion  = newVersion;
      mockUpdate.hasUpdate      = false;
      mockUpdate.state          = 'idle';
      _rebooting = false;
    }, 4000);
  }, 1500);
}

// Simulate a peer running through the full OTA update cycle.
function simulatePeerUpdateCycle(peer) {
  const newVersion = MOCK_SELF.version;
  peer.fwState = 'checking';
  broadcastPeers();
  setTimeout(() => {
    peer.fwState = 'downloading';
    broadcastPeers();
    setTimeout(() => {
      peer.fwState = 'done';
      peer.version = newVersion;
      broadcastPeers();
      setTimeout(() => {
        peer.fwState = 'idle';
        broadcastPeers();
      }, 3000);
    }, 5000);
  }, 2000);
}

// Simulate a peer checking for updates (no install).
function simulatePeerCheckCycle(peer) {
  peer.fwState = 'checking';
  broadcastPeers();
  setTimeout(() => {
    peer.fwState = 'idle';
    broadcastPeers();
  }, 2000);
}

// Resolve the simulated boot check after 3 s — exercises the poll-while-checking
// path and causes the update badge to appear without any user action.
setTimeout(() => {
  mockUpdate.state         = 'idle';
  mockUpdate.latestVersion = '9999.99.99.0';
  mockUpdate.hasUpdate     = true;
  MOCK_SELF.fwState        = 'idle';
  broadcastPeers();
}, 3000);

const app = express();
app.set('trust proxy', true);
app.use(express.json());
app.use('/auth', authRouter);
app.use(requireAuth);
app.use(express.static(DATA_DIR));

app.use('/api', (req, res, next) => {
  if (_rebooting) return res.status(503).json({ error: 'device rebooting' });
  next();
});

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

app.get('/api/peers', (_req, res) => res.json({ self: selfWithLights(), peers: MOCK_PEERS }));
app.post('/api/peers/setgroup', (req, res) => {
  const { mac, lightIndex, groupId } = req.body || {};
  // Mirrors WebServer.h::_setRemoteGroup: only self-mac assignments are
  // persisted locally; remote peers would relay over the mesh on real hardware.
  if (mac === MOCK_SELF.mac) {
    const light = mockLights.find(l => l.index === lightIndex);
    if (light) light.groupId = groupId;
    broadcastPeers();
  }
  res.json({ ok: true });
});
app.post('/api/peers/setscenesync', (_req, res) => res.json({ ok: true }));
app.post('/api/peers/pushconfig',      (_req, res) => res.json({ ok: true }));
app.post('/api/peers/triggerupdate', (req, res) => {
  const { mac } = req.body || {};
  const peer = MOCK_PEERS.find(p => p.mac === mac);
  if (peer && !peer.wifiConnected) return res.status(409).json({ error: 'peer not connected to WiFi' });
  res.json({ ok: true });
  if (peer) simulatePeerUpdateCycle(peer);
});
app.post('/api/peers/checkupdate', (req, res) => {
  const { mac } = req.body || {};
  const peer = MOCK_PEERS.find(p => p.mac === mac);
  if (peer && !peer.wifiConnected) return res.status(409).json({ error: 'peer not connected to WiFi' });
  res.json({ ok: true });
  if (peer) simulatePeerCheckCycle(peer);
});

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

app.get('/api/lights', (_req, res) => res.json({ lights: mockLights, maxLights: 4 }));
app.post('/api/lights/add', (req, res) => {
  const free = [0,1,2,3].find(i => !mockLights.find(l => l.index === i));
  if (free === undefined) return res.status(400).json({ error: 'light limit reached' });
  const { name = '', ledType = 0, dataPin = 13, clockPin = 14, width = 1, height = 1, matrixStart = 0, matrixDir = 0, matrixSerpentine = false, wrapWidth = false, wrapHeight = false, groupId = 0 } = req.body || {};
  mockLights.push({ index: free, name, ledType, dataPin, clockPin, width, height, matrixStart, matrixDir, matrixSerpentine, wrapWidth, wrapHeight, groupId });
  res.json({ ok: true, index: free });
});
app.post('/api/lights/update', (req, res) => {
  const { index, ...fields } = req.body || {};
  const light = mockLights.find(l => l.index === index);
  if (!light) return res.status(404).json({ error: 'not found' });
  Object.assign(light, fields);
  res.json({ ok: true });
});
app.post('/api/lights/delete', (req, res) => {
  const { index } = req.body || {};
  const idx = mockLights.findIndex(l => l.index === index);
  if (idx === -1) return res.status(404).json({ error: 'not found' });
  mockLights.splice(idx, 1);
  res.json({ ok: true });
});
app.post('/api/lights/test', (req, res) => {
  const { index } = req.body || {};
  const light = mockLights.find(l => l.index === index);
  if (!light) return res.status(404).json({ error: 'not found' });
  if (light.height < 2) return res.status(400).json({ error: 'not a matrix' });
  res.json({ ok: true });
});

app.post('/api/groups/create',  (_req, res) => res.json({ ok: true }));
app.post('/api/groups/update',  (req, res) => {
  const { id, ...fields } = req.body || {};
  const group = MOCK_CONFIG.groups.find(g => g.id === id);
  if (!group) return res.status(404).json({ error: 'not found' });
  Object.assign(group, fields);
  res.json({ ok: true });
});
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
  mockUpdate.state  = 'checking';
  mockUpdate.error  = null;
  MOCK_SELF.fwState = 'checking';
  broadcastPeers();
  res.json({ ok: true });
  setTimeout(() => {
    mockUpdate.state         = 'idle';
    mockUpdate.latestVersion = '1.2.0';
    mockUpdate.hasUpdate     = true;
    MOCK_SELF.fwState        = 'idle';
    broadcastPeers();
  }, 2000);
});

app.post('/api/update/apply', (_req, res) => {
  if (!mockUpdate.hasUpdate) {
    return res.status(400).json({ error: 'no update available' });
  }
  mockUpdate.state    = 'downloading';
  mockUpdate.progress = 0;
  MOCK_SELF.fwState   = 'downloading';
  broadcastPeers();
  res.json({ ok: true });
  let p = 0;
  const iv = setInterval(() => {
    p += 10;
    mockUpdate.progress = p;
    if (p >= 100) {
      clearInterval(iv);
      mockUpdate.state = 'done';
      simulateReboot(mockUpdate.latestVersion);
    }
  }, 500);
});

app.post('/api/update/trigger', (_req, res) => {
  res.json({ ok: true });
  if (mockUpdate.hasUpdate) {
    // Simulate the apply flow
    mockUpdate.state    = 'downloading';
    mockUpdate.progress = 0;
    MOCK_SELF.fwState   = 'downloading';
    broadcastPeers();
    let p = 0;
    const iv = setInterval(() => {
      p += 10;
      mockUpdate.progress = p;
      if (p >= 100) {
        clearInterval(iv);
        mockUpdate.state = 'done';
        simulateReboot(mockUpdate.latestVersion);
      }
    }, 500);
  } else {
    // Simulate check then apply
    mockUpdate.state  = 'checking';
    MOCK_SELF.fwState = 'checking';
    broadcastPeers();
    setTimeout(() => {
      mockUpdate.state         = 'idle';
      mockUpdate.latestVersion = '9999.99.99.0';
      mockUpdate.hasUpdate     = true;
      MOCK_SELF.fwState        = 'idle';
      broadcastPeers();
    }, 2000);
  }
});

app.get('/*path', (_req, res) => res.sendFile(join(DATA_DIR, 'index.html')));

const server = http.createServer(app);

const wss = new WebSocketServer({ server, path: '/ws' });
wss.on('connection', ws => {
  if (_rebooting) { ws.terminate(); return; }
  const send = data => ws.send(JSON.stringify(data));
  send({ t: 'log', l: 'I', m: 'Mock server connected' });
  send({ t: 'log', l: 'I', m: 'This is a development mock — no hardware attached' });
  send({ t: 'peers', self: selfWithLights(), peers: MOCK_PEERS });
  send({ t: 'groups', list: MOCK_CONFIG.groups });
});

const PORT = process.env.PORT || 8080;
server.listen(PORT, () => console.log(`Battery Light mock server: http://localhost:${PORT}`));
