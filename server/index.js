import http from 'http';
import fs from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import express from 'express';
import { WebSocketServer } from 'ws';
import { authRouter, requireAuth } from './auth.js';

const DATA_DIR = join(dirname(fileURLToPath(import.meta.url)), '..', 'data');

const mockLights = [
  { index: 0, name: 'Living room', ledType: 0, colorOrder: 2, dataPin: 13, clockPin: 14, width: 1,  height: 1, matrixStart: 0, matrixDir: 0, matrixSerpentine: false, wrapWidth: false, wrapHeight: false, groupId: 0, brightnessOverrideEnabled: false, brightnessOverride: 255 },
  { index: 1, name: 'Bedroom',     ledType: 1, colorOrder: 0, dataPin: 25, clockPin: 26, width: 8,  height: 8, matrixStart: 2, matrixDir: 1, matrixSerpentine: false, wrapWidth: false, wrapHeight: false, groupId: 1, brightnessOverrideEnabled: true,  brightnessOverride: 120 },
  { index: 2, name: 'Patio',       ledType: 0, colorOrder: 2, dataPin: 27, clockPin: 32, width: 12, height: 1, matrixStart: 0, matrixDir: 0, matrixSerpentine: false, wrapWidth: true,  wrapHeight: false, groupId: 2, brightnessOverrideEnabled: false, brightnessOverride: 255 },
];

// The real device derives self.lights in /api/peers straight from the live
// hardware config on every request (see WebServer.h::_buildPeersJson), so it
// can never drift. Mirror that here instead of keeping a second static copy.
function mockSelfLights() {
  return mockLights.map(({ index, name, groupId, ledType, width, height, wrapWidth, brightnessOverrideEnabled, brightnessOverride }) =>
    ({ index, name, groupId, ledType, width, height, wrapWidth, brightnessOverrideEnabled, brightnessOverride }));
}

const MOCK_CONFIG = {
  deviceName: 'Mock Device',
  otaPort: 3232,
  otaEnabled: true,
  logLevel: 1,
  sceneSyncEnabled: true,
  checkUpdateOnStartup: false,
  wifiSingleClientMode: false,
  batteryHwSupported: true,
  batteryMonitoringEnabled: true,
  mqttHost: 'mqtt.local',
  mqttPort: 1883,
  mqttUser: 'lights',
  version: 'mock',
  mac: '11:22:33:44:55:66',
  githubRepo: 'Variour/batterylight',
  timezone: 'CET-1CEST,M3.5.0,M10.5.0/3', // Europe/Berlin
  lights: mockLights,
  groups: [
    { id: 0, name: 'Default',     exists: true, mode: 0, sceneId: '',          pattern: 0, r: 255, g: 200, b: 80,  brightness: 200, speed: 1, seq: 0, syncEnabled: true,  transitionEnabled: false, sceneUniformColor: false, transitionTime: 2.0, frameDuration: 1.0, proximityScale: 1.0, morphEnabled: false, gradientStopCount: 0, text: '', textAnimation: 0, time24h: true },
    { id: 1, name: 'Scene Group', exists: true, mode: 1, sceneId: '0002ee38f7ce6ab7acd6a859', pattern: 0, r: 255, g: 100, b: 50, brightness: 180, speed: 1, seq: 0, syncEnabled: false, transitionEnabled: false, sceneUniformColor: false, transitionTime: 2.0, frameDuration: 1.0, proximityScale: 1.0, morphEnabled: false, gradientStopCount: 0, text: '', textAnimation: 0, time24h: true },
    { id: 2, name: 'Gradient Group', exists: true, mode: 3, sceneId: '0002ee38f7ce6ab7acd6a859', pattern: 0, r: 255, g: 255, b: 255, brightness: 220, speed: 1, seq: 0, syncEnabled: true, transitionEnabled: false, sceneUniformColor: false, transitionTime: 2.0, frameDuration: 1.0, proximityScale: 1.0, morphEnabled: true, gradientStopCount: 5, text: '', textAnimation: 0, time24h: true },
  ],
};

// Mirrors SoundHardwareConfig shape from Config.h/WebServer.h::serializeSound.
// 255 mirrors Config.h::SOUND_PIN_UNUSED (an unconfigured/not-present pin).
const SOUND_PIN_UNUSED = 255;
const mockSounds = [
  { index: 0, name: 'Speaker', chip: 0, i2cSdaPin: 21, i2cSclPin: 22, i2cAddress: 0x18,
    i2sMclkPin: SOUND_PIN_UNUSED, i2sBclkPin: 15, i2sWsPin: 16, i2sDoutPin: 17,
    paEnablePin: 23, paEnableActiveHigh: true },
];

// Mirrors ButtonHardwareConfig/ButtonAction shape from WebServer.h::serializeButton.
// One pre-populated button so the Buttons UI has something to show/edit by default.
const mockButtons = [
  { index: 0, name: 'Wall switch', pin: 4, activeLow: true, exists: true,
    onShortPress:  { action: 1, groupId: 0, lightIndex: 0, numberValue: 20, stringValue: '', r: 255, g: 255, b: 255 }, // BrightnessStep
    onLongPress:   { action: 6, groupId: 0, lightIndex: 0, numberValue: 0,  stringValue: '', r: 255, g: 255, b: 255 }, // PatternNext
    onDoubleClick: { action: 3, groupId: 0, lightIndex: 0, numberValue: 0,  stringValue: '', r: 0,   g: 150, b: 255 }, // ColorSet
  },
  { index: 1, name: 'Nightstand', pin: 5, activeLow: true, exists: true,
    onShortPress:  { action: 12, groupId: 1, lightIndex: 0, numberValue: 0, stringValue: '0002ee38f7ce6ab7acd6a859', r: 255, g: 255, b: 255 }, // SceneSet
    onLongPress:   { action: 5,  groupId: 1, lightIndex: 0, numberValue: 1, stringValue: '', r: 255, g: 255, b: 255 }, // ModeSet → Scene
    onDoubleClick: { action: 29, groupId: 0, lightIndex: 1, numberValue: 20, stringValue: '', r: 255, g: 255, b: 255 }, // LightBrightnessOverrideStep on Bedroom
  },
];

const MOCK_SELF  = { name: 'Mock Device',   mac: '11:22:33:44:55:66', online: true,  sceneSyncEnabled: true,  wifiConnected: true,  hasWifiNetworks: true,  wifiConnecting: false, channel: 6, channelSearching: false, version: '2026.06.27.0', fwState: 'checking', batteryPresent: true, batteryPercent: 82, batteryCharging: false };
const MOCK_PEERS = [
  { name: 'Mock Light 2', mac: '22:33:44:55:66:77', lights: [{ index: 0, name: 'Kitchen', groupId: 0 }], online: true,  rssi: -65, sceneSyncEnabled: true,  wifiConnected: true,  hasWifiNetworks: true,  wifiConnecting: false, version: '2026.01.01.0', fwState: 'idle', batteryPresent: true,  batteryPercent: 46, batteryCharging: false },
  { name: 'Mock Light 3', mac: '33:44:55:66:77:88', lights: [{ index: 0, name: 'Hallway', groupId: 1 }, { index: 1, name: 'Closet', groupId: 0 }], online: true,  rssi: -80, sceneSyncEnabled: false, wifiConnected: false, hasWifiNetworks: true,  wifiConnecting: false, version: '2026.01.01.0', fwState: 'idle', batteryPresent: true,  batteryPercent: 12, batteryCharging: false },
  { name: 'Mock Light 4', mac: '44:55:66:77:88:99', lights: [{ index: 0, name: '', groupId: 0 }], online: true,  rssi: -55, sceneSyncEnabled: true,  wifiConnected: true,  hasWifiNetworks: false, wifiConnecting: false, version: '2026.06.27.0', fwState: 'idle', batteryPresent: true,  batteryPercent: 97, batteryCharging: true  },
  { name: 'Mock Light 5', mac: '55:66:77:88:99:aa', lights: [{ index: 0, name: 'Garage', groupId: 0 }], online: true,  rssi: -70, sceneSyncEnabled: true,  wifiConnected: false, hasWifiNetworks: true,  wifiConnecting: true,  version: '2026.01.01.0', fwState: 'idle', batteryPresent: false, batteryPercent: 0,  batteryCharging: false },
];

const wifiNetworks = [
  { ssid: 'HomeNetwork', password: 'secret1' },
  { ssid: 'WorkWifi',    password: 'secret2' },
  { ssid: 'GuestWifi',   password: 'secret3' },
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
} catch { /* data/sc may not exist */ }

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
  const msg = JSON.stringify({
    t: 'peers',
    self: selfWithLights(),
    peers: MOCK_PEERS,
    wifiSingleClientMode: MOCK_CONFIG.wifiSingleClientMode,
  });
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
app.post('/api/config', (req, res) => {
  // Mirrors WebServer.h::_postConfig, which never re-exposes write-only
  // secrets (mqttPassword, githubToken) via GET /api/config.
  const rest = { ...req.body };
  delete rest.mqttPassword;
  delete rest.githubToken;
  Object.assign(MOCK_CONFIG, rest);
  res.json({ ok: true });
});

// Mirrors WebServer.h::_clearMqtt — removes the broker config. The real
// device also clears retained MQTT topics on the broker itself, which has
// no equivalent in this HTTP/WS mock.
app.post('/api/mqtt/clear', (_req, res) => {
  MOCK_CONFIG.mqttHost = '';
  MOCK_CONFIG.mqttPort = 1883;
  MOCK_CONFIG.mqttUser = '';
  res.json({ ok: true });
});

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
app.post('/api/wifi/move', (req, res) => {
  const { ssid, direction } = req.body;
  if (!ssid) return res.status(400).json({ error: 'ssid required' });
  if (direction !== 'up' && direction !== 'down') {
    return res.status(400).json({ error: 'direction must be up or down' });
  }
  const i = wifiNetworks.findIndex(n => n.ssid === ssid);
  const j = i + (direction === 'up' ? -1 : 1);
  if (i === -1 || j < 0 || j >= wifiNetworks.length) {
    return res.status(400).json({ error: 'cannot move' });
  }
  [wifiNetworks[i], wifiNetworks[j]] = [wifiNetworks[j], wifiNetworks[i]];
  res.json({ ok: true });
});

app.get('/api/peers', (_req, res) => res.json({
  self: selfWithLights(),
  peers: MOCK_PEERS,
  wifiSingleClientMode: MOCK_CONFIG.wifiSingleClientMode,
}));
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
// Mirrors WebServer.h::_peerUpdateRequest: an online standby candidate
// (single-client mode + hasWifiNetworks) can still connect on demand, but an
// offline peer must still be rejected even if it would otherwise qualify.
function otaPeerError(peer) {
  if (!peer) return null;
  if (peer.online === false) return 'peer offline';
  if (peer.wifiConnected) return null;
  return (MOCK_CONFIG.wifiSingleClientMode && peer.hasWifiNetworks)
    ? null
    : 'peer not connected to WiFi';
}
app.post('/api/peers/triggerupdate', (req, res) => {
  const { mac } = req.body || {};
  const peer = MOCK_PEERS.find(p => p.mac === mac);
  const error = otaPeerError(peer);
  if (error) return res.status(409).json({ error });
  res.json({ ok: true });
  if (peer) simulatePeerUpdateCycle(peer);
});
app.post('/api/peers/checkupdate', (req, res) => {
  const { mac } = req.body || {};
  const peer = MOCK_PEERS.find(p => p.mac === mac);
  const error = otaPeerError(peer);
  if (error) return res.status(409).json({ error });
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
  const { name = '', ledType = 0, colorOrder, dataPin = 13, clockPin = 14, width = 1, height = 1, matrixStart = 0, matrixDir = 0, matrixSerpentine = false, wrapWidth = false, wrapHeight = false, groupId = 0 } = req.body || {};
  // Mirrors Config.h::defaultColorOrder: WS2812B (0) is conventionally wired GRB (2), WS2801 (1) RGB (0).
  const resolvedColorOrder = colorOrder !== undefined ? colorOrder : (ledType === 0 ? 2 : 0);
  mockLights.push({ index: free, name, ledType, colorOrder: resolvedColorOrder, dataPin, clockPin, width, height, matrixStart, matrixDir, matrixSerpentine, wrapWidth, wrapHeight, groupId });
  res.json({ ok: true, index: free });
});
app.post('/api/lights/update', (req, res) => {
  const { index, ...fields } = req.body || {};
  const light = mockLights.find(l => l.index === index);
  if (!light) return res.status(404).json({ error: 'not found' });
  Object.assign(light, fields);
  res.json({ ok: true });
  // Mirrors WebServer.h::_updateLight: a brightness override change is a live
  // update (no reboot) that pushes to the dashboard via WS, unlike other
  // hardware-config fields on this endpoint (which trigger ESP.restart() on
  // real hardware and so never round-trip back to the dashboard mid-session).
  if ('brightnessOverrideEnabled' in fields || 'brightnessOverride' in fields) broadcastPeers();
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
app.post('/api/lights/testcolor', (req, res) => {
  const { index } = req.body || {};
  const light = mockLights.find(l => l.index === index);
  if (!light) return res.status(404).json({ error: 'not found' });
  res.json({ ok: true });
});

function soundPins(s) {
  return [s.i2cSdaPin, s.i2cSclPin, s.i2sMclkPin, s.i2sBclkPin, s.i2sWsPin, s.i2sDoutPin, s.paEnablePin]
    .filter(p => p !== SOUND_PIN_UNUSED);
}

function isButtonPinInUse(pin, excludeIndex) {
  if (mockLights.some(l => l.dataPin === pin || l.clockPin === pin)) return true;
  if (mockSounds.some(s => soundPins(s).includes(pin))) return true;
  return mockButtons.some(b => b.index !== excludeIndex && b.pin === pin);
}

// Mirrors WebServer.h::_soundPinConflict: every non-unused pin on `s` must be
// mutually distinct and not already claimed by a light, button, or another
// sound output. Pass s.index as excludeIndex when validating an update.
function soundPinConflict(s, excludeIndex) {
  const pins = soundPins(s);
  if (new Set(pins).size !== pins.length) return 'duplicate pin within sound config';
  if (mockLights.some(l => pins.includes(l.dataPin) || pins.includes(l.clockPin))) return 'pin already in use';
  if (mockButtons.some(b => pins.includes(b.pin))) return 'pin already in use';
  if (mockSounds.some(other => other.index !== excludeIndex && soundPins(other).some(p => pins.includes(p))))
    return 'pin already in use';
  return null;
}

const MAX_SOUNDS = 1;

app.get('/api/sounds', (_req, res) => res.json({ sounds: mockSounds, maxSounds: MAX_SOUNDS }));
app.post('/api/sounds/add', (req, res) => {
  const free = Array.from({ length: MAX_SOUNDS }, (_, i) => i).find(i => !mockSounds.find(s => s.index === i));
  if (free === undefined) return res.status(400).json({ error: 'sound limit reached' });
  const {
    name = '', chip = 0, i2cSdaPin = SOUND_PIN_UNUSED, i2cSclPin = SOUND_PIN_UNUSED,
    i2cAddress = 0x18, i2sMclkPin = SOUND_PIN_UNUSED, i2sBclkPin = SOUND_PIN_UNUSED,
    i2sWsPin = SOUND_PIN_UNUSED, i2sDoutPin = SOUND_PIN_UNUSED, paEnablePin = SOUND_PIN_UNUSED,
    paEnableActiveHigh = true,
  } = req.body || {};
  if ([i2cSdaPin, i2cSclPin, i2sBclkPin, i2sWsPin, i2sDoutPin].includes(SOUND_PIN_UNUSED)) {
    return res.status(400).json({ error: 'missing required pin' });
  }
  const sound = { index: free, name, chip, i2cSdaPin, i2cSclPin, i2cAddress, i2sMclkPin, i2sBclkPin, i2sWsPin, i2sDoutPin, paEnablePin, paEnableActiveHigh };
  const conflict = soundPinConflict(sound, free);
  if (conflict) return res.status(400).json({ error: conflict });
  mockSounds.push(sound);
  res.json({ ok: true, index: free });
});
app.post('/api/sounds/update', (req, res) => {
  const { index, ...fields } = req.body || {};
  const sound = mockSounds.find(s => s.index === index);
  if (!sound) return res.status(404).json({ error: 'not found' });
  const candidate = { ...sound, ...fields };
  const hwChanged = Object.keys(fields).some(k => k !== 'name');
  if (hwChanged) {
    if ([candidate.i2cSdaPin, candidate.i2cSclPin, candidate.i2sBclkPin, candidate.i2sWsPin, candidate.i2sDoutPin].includes(SOUND_PIN_UNUSED)) {
      return res.status(400).json({ error: 'missing required pin' });
    }
    const conflict = soundPinConflict(candidate, index);
    if (conflict) return res.status(400).json({ error: conflict });
  }
  Object.assign(sound, fields);
  res.json({ ok: true });
});
app.post('/api/sounds/delete', (req, res) => {
  const { index } = req.body || {};
  const idx = mockSounds.findIndex(s => s.index === index);
  if (idx === -1) return res.status(404).json({ error: 'not found' });
  mockSounds.splice(idx, 1);
  res.json({ ok: true });
});
app.post('/api/sounds/test', (req, res) => {
  const { index } = req.body || {};
  const sound = mockSounds.find(s => s.index === index);
  if (!sound) return res.status(404).json({ error: 'not found' });
  res.json({ ok: true });
});

app.get('/api/buttons', (_req, res) => res.json({ buttons: mockButtons, maxButtons: 4 }));
app.post('/api/buttons/add', (req, res) => {
  const free = [0,1,2,3].find(i => !mockButtons.find(b => b.index === i));
  if (free === undefined) return res.status(400).json({ error: 'button limit reached' });
  const { name = '', pin = 0, activeLow = true, onShortPress, onLongPress, onDoubleClick } = req.body || {};
  if (isButtonPinInUse(pin, free)) return res.status(400).json({ error: 'pin already in use' });
  const blankAction = () => ({ action: 0, groupId: 0, lightIndex: 0, numberValue: 0, stringValue: '', r: 255, g: 255, b: 255 });
  mockButtons.push({
    index: free, name, pin, activeLow, exists: true,
    onShortPress:  onShortPress  || blankAction(),
    onLongPress:   onLongPress   || blankAction(),
    onDoubleClick: onDoubleClick || blankAction(),
  });
  res.json({ ok: true, index: free });
});
app.post('/api/buttons/update', (req, res) => {
  const { index, ...fields } = req.body || {};
  const button = mockButtons.find(b => b.index === index);
  if (!button) return res.status(404).json({ error: 'not found' });
  if (fields.pin !== undefined && isButtonPinInUse(fields.pin, index)) {
    return res.status(400).json({ error: 'pin already in use' });
  }
  Object.assign(button, fields);
  res.json({ ok: true });
});
app.post('/api/buttons/delete', (req, res) => {
  const { index } = req.body || {};
  const idx = mockButtons.findIndex(b => b.index === index);
  if (idx === -1) return res.status(404).json({ error: 'not found' });
  mockButtons.splice(idx, 1);
  res.json({ ok: true });
});

const MAX_GROUPS = 8; // mirrors Config::MAX_GROUPS; id 0 is reserved for Default

app.post('/api/groups/create',  (req, res) => {
  const free = Array.from({ length: MAX_GROUPS - 1 }, (_, i) => i + 1)
    .find(i => !MOCK_CONFIG.groups.find(g => g.id === i));
  if (free === undefined) return res.status(400).json({ error: 'group limit reached' });
  const { name = 'New Group' } = req.body || {};
  MOCK_CONFIG.groups.push({
    id: free, name, exists: true, mode: 0, sceneId: '', pattern: 0, r: 255, g: 255, b: 255,
    brightness: 255, speed: 1, seq: 0, syncEnabled: true, transitionEnabled: false,
    sceneUniformColor: false, transitionTime: 2.0, frameDuration: 1.0, proximityScale: 1.0,
    morphEnabled: false, gradientStopCount: 0, text: '', textAnimation: 0, time24h: true,
  });
  res.json({ ok: true, id: free });
});
app.post('/api/groups/update',  (req, res) => {
  const { id, ...fields } = req.body || {};
  const group = MOCK_CONFIG.groups.find(g => g.id === id);
  if (!group) return res.status(404).json({ error: 'not found' });
  Object.assign(group, fields);
  res.json({ ok: true });
});
app.post('/api/groups/delete',  (req, res) => {
  const { id } = req.body || {};
  if (id === 0) return res.status(400).json({ error: 'cannot delete Default' });
  const idx = MOCK_CONFIG.groups.findIndex(g => g.id === id);
  if (idx === -1) return res.status(404).json({ error: 'not found' });
  MOCK_CONFIG.groups.splice(idx, 1);
  for (const l of mockLights) if (l.groupId === id) l.groupId = 0;
  res.json({ ok: true });
});
app.post('/api/reset',          (_req, res) => res.json({ ok: true }));
app.post('/api/mesh/search',    (_req, res) => res.json({ ok: true }));
app.post('/api/mesh/wifipolicy', (req, res) => {
  MOCK_CONFIG.wifiSingleClientMode = !!(req.body || {}).enabled;
  res.json({ ok: true });
  broadcastPeers();
});
app.post('/api/mesh/wifiretry', (_req, res) => res.json({ ok: true }));

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
  send({ t: 'peers', self: selfWithLights(), peers: MOCK_PEERS, wifiSingleClientMode: MOCK_CONFIG.wifiSingleClientMode });
  send({ t: 'groups', list: MOCK_CONFIG.groups });
});

const PORT = process.env.PORT || 8080;
server.listen(PORT, () => console.log(`Battery Light mock server: http://localhost:${PORT}`));

export { app, server };
