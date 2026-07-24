import http from 'http';
import fs from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import express from 'express';
import { WebSocketServer } from 'ws';
import { authRouter, requireAuth } from './auth.js';

const DATA_DIR = join(dirname(fileURLToPath(import.meta.url)), '..', 'data');

const mockLights = [
  { index: 0, name: 'Living room', ledType: 0, colorOrder: 2, dataPin: 13, clockPin: 14, width: 1,  height: 1, matrixStart: 0, matrixDir: 0, matrixSerpentine: false, wrapWidth: false, wrapHeight: false, groupId: 0, brightnessOverrideEnabled: false, brightnessOverride: 255, brightnessLimit: 255, brightnessScale: 255 },
  { index: 1, name: 'Bedroom',     ledType: 1, colorOrder: 0, dataPin: 25, clockPin: 26, width: 8,  height: 8, matrixStart: 2, matrixDir: 1, matrixSerpentine: false, wrapWidth: false, wrapHeight: false, groupId: 1, brightnessOverrideEnabled: true,  brightnessOverride: 120, brightnessLimit: 255, brightnessScale: 255 },
  { index: 2, name: 'Patio',       ledType: 0, colorOrder: 2, dataPin: 27, clockPin: 32, width: 12, height: 1, matrixStart: 0, matrixDir: 0, matrixSerpentine: false, wrapWidth: true,  wrapHeight: false, groupId: 2, brightnessOverrideEnabled: false, brightnessOverride: 255, brightnessLimit: 128, brightnessScale: 180 },
];

// The real device derives self.lights in /api/peers straight from the live
// hardware config on every request (see WebServer.h::_buildPeersJson), so it
// can never drift. Mirror that here instead of keeping a second static copy.
function mockSelfLights() {
  return mockLights.map(({ index, name, groupId, ledType, width, height, wrapWidth, brightnessOverrideEnabled, brightnessOverride }) =>
    ({ index, name, groupId, ledType, width, height, wrapWidth, brightnessOverrideEnabled, brightnessOverride }));
}

// Same "derive from the live hardware config, never a second static copy"
// approach as mockSelfLights(), for the one sound output a device may have.
function mockSelfSound() {
  const sound = mockSounds.find(s => s.exists);
  return sound ? { name: sound.name, audioGroupId: sound.audioGroupId,
    volumeOverrideEnabled: sound.volumeOverrideEnabled, volume: effectiveSoundVolume(sound) } : null;
}

// Mirrors Config::effectiveSoundVolume: own override if enabled, else the
// audio group's shared volume.
function effectiveSoundVolume(sound) {
  if (sound.volumeOverrideEnabled) return sound.volume;
  const group = mockAudioGroups.find(g => g.id === sound.audioGroupId);
  return group ? group.volume : sound.volume;
}

const MOCK_CONFIG = {
  deviceName: 'Mock Device',
  otaPort: 3232,
  otaEnabled: true,
  logLevel: 2, // Info
  sceneSyncEnabled: true,
  checkUpdateOnStartup: false,
  eventLogLimit: 10,
  wifiSingleClientMode: false,
  batteryHwSupported: true,
  batteryMonitoringEnabled: true,
  prOtaBoardSupported: true,
  prOtaEnabled: true,
  prTrack: '',
  i2cSdaPin: 21,
  i2cSclPin: 22,
  expanderChip: 1,
  expanderAddress: 0x20,
  mqttHost: 'mqtt.local',
  mqttPort: 1883,
  mqttUser: 'lights',
  version: 'mock',
  mac: '11:22:33:44:55:66',
  githubRepo: 'Variour/lightwitch',
  timezone: 'CET-1CEST,M3.5.0,M10.5.0/3', // Europe/Berlin
  lights: mockLights,
  groups: [
    { id: 0, name: 'Default',     exists: true, mode: 0, sceneId: '',          pattern: 0, r: 255, g: 200, b: 80,  brightness: 200, speed: 1, seq: 0, syncEnabled: true,  proximityScale: 1.0, morphEnabled: false, gradientStopCount: 0, text: '', textAnimation: 0, time24h: true },
    { id: 1, name: 'Scene Group', exists: true, mode: 1, sceneId: '0002ee38f7ce6ab7acd6a859', pattern: 0, r: 255, g: 100, b: 50, brightness: 180, speed: 1, seq: 0, syncEnabled: false, proximityScale: 1.0, morphEnabled: false, gradientStopCount: 0, text: '', textAnimation: 0, time24h: true },
    { id: 2, name: 'Gradient Group', exists: true, mode: 3, sceneId: '0002ee38f7ce6ab7acd6a859', pattern: 0, r: 255, g: 255, b: 255, brightness: 220, speed: 1, seq: 0, syncEnabled: true, proximityScale: 1.0, morphEnabled: true, gradientStopCount: 5, text: '', textAnimation: 0, time24h: true },
  ],
};

// Mirrors SoundHardwareConfig shape from Config.h/WebServer.h::serializeSound.
// 255 mirrors Config.h::PIN_UNUSED (an unconfigured/not-present pin). The I2C
// control bus (sda/scl) and the expander (chip/address) are both device-wide
// now — see MOCK_CONFIG.i2cSdaPin/i2cSclPin/expanderChip/expanderAddress —
// not per-sound/per-button fields.
const PIN_UNUSED = 255;
// Mirrors Config.h::SOUND_VOLUME_MIN/MAX.
const SOUND_VOLUME_MIN = 50;
const SOUND_VOLUME_MAX = 200;
const clampVolume = v => Math.max(SOUND_VOLUME_MIN, Math.min(SOUND_VOLUME_MAX, Number(v)));
// paViaExpander: false = paEnablePin is a native GPIO, true = it's a pin
// index (0-15) on the device's expander instead — mirrors
// SoundHardwareConfig::paViaExpander.
const mockSounds = [
  { index: 0, name: 'Speaker', chip: 0, i2cAddress: 0x18,
    i2sMclkPin: PIN_UNUSED, i2sBclkPin: 15, i2sWsPin: 16, i2sDoutPin: 17,
    paEnablePin: 8, paEnableActiveHigh: true, paViaExpander: true,
    audioGroupId: 0, volumeOverrideEnabled: false, volume: 200, exists: true },
];

// Mirrors AudioGroupConfig from Config.h — id 0 (Default) always exists,
// same as light groups. volume is the group's shared volume, followed by
// every member device without its own volumeOverrideEnabled.
const mockAudioGroups = [
  { id: 0, name: 'Default', exists: true, volume: 200 },
  { id: 1, name: 'Living Room Speakers', exists: true, volume: 150 },
];

// Mirrors PlaylistManager's storage shape (id/name/loop/files). files
// reference names from mockStorage below.
const mockPlaylists = new Map([
  ['mockpl1', { id: 'mockpl1', name: 'Morning Mix', loop: false, files: ['doorbell.wav', 'alarm.wav'] }],
]);

// Mirrors ButtonHardwareConfig/ButtonAction shape from WebServer.h::serializeButton.
// viaExpander mirrors SoundHardwareConfig::paViaExpander (see mockSounds
// above) — button 2 shares the device expander with the sound's PA-enable
// pin (8) but uses a different pin index (9) to exercise that overlap
// without conflicting, same as the Waveshare board's real KEY1 wiring.
const mockButtons = [
  { index: 0, name: 'Wall switch', pin: 4, activeLow: true, viaExpander: false, exists: true,
    onShortPress:  { action: 1, groupId: 0, lightIndex: 0, numberValue: 20, stringValue: '', r: 255, g: 255, b: 255 }, // BrightnessStep
    onLongPress:   { action: 6, groupId: 0, lightIndex: 0, numberValue: 0,  stringValue: '', r: 255, g: 255, b: 255 }, // PatternNext
    onDoubleClick: { action: 3, groupId: 0, lightIndex: 0, numberValue: 0,  stringValue: '', r: 0,   g: 150, b: 255 }, // ColorSet
  },
  { index: 1, name: 'Nightstand', pin: 5, activeLow: true, viaExpander: false, exists: true,
    onShortPress:  { action: 12, groupId: 1, lightIndex: 0, numberValue: 0, stringValue: '0002ee38f7ce6ab7acd6a859', r: 255, g: 255, b: 255 }, // SceneSet
    onLongPress:   { action: 5,  groupId: 1, lightIndex: 0, numberValue: 1, stringValue: '', r: 255, g: 255, b: 255 }, // ModeSet → Scene
    onDoubleClick: { action: 25, groupId: 0, lightIndex: 1, numberValue: 20, stringValue: '', r: 255, g: 255, b: 255 }, // LightBrightnessOverrideStep on Bedroom
  },
  { index: 2, name: 'KEY1 (expander)', pin: 9, activeLow: true, viaExpander: true, exists: true,
    onShortPress:  { action: 9, groupId: 0, lightIndex: 0, numberValue: 0, stringValue: '', r: 255, g: 255, b: 255 }, // SceneNext
    onLongPress:   { action: 0, groupId: 0, lightIndex: 0, numberValue: 0, stringValue: '', r: 255, g: 255, b: 255 },
    onDoubleClick: { action: 0, groupId: 0, lightIndex: 0, numberValue: 0, stringValue: '', r: 255, g: 255, b: 255 },
  },
];

// Mirrors AutomationBinding/AutomationRule shape from WebServer.h::serializeAutomationBinding
// (issue #439). triggerType 0 = GenericEvent (the only trigger type today).
const MAX_AUTOMATION_BINDINGS = 8;
const MAX_RULES_PER_BINDING = 4;
const MAX_ACTIONS_PER_RULE = 3;
const blankAutomationAction = () => ({ action: 0, groupId: 0, lightIndex: 0, numberValue: 0, stringValue: '', r: 255, g: 255, b: 255 });
const blankAutomationRule = () => ({ valueMin: 0, valueMax: 65535, exists: false, actions: Array.from({ length: MAX_ACTIONS_PER_RULE }, blankAutomationAction) });

const mockAutomations = [
  { index: 0, triggerType: 0, eventType: 'buzz.press', exists: true,
    rules: [
      { valueMin: 0, valueMax: 1, exists: true, actions: [
          { action: 1, groupId: 0, lightIndex: 0, numberValue: -20, stringValue: '', r: 255, g: 255, b: 255 }, // BrightnessStep down
          blankAutomationAction(), blankAutomationAction(),
        ] },
      { valueMin: 2, valueMax: 65535, exists: true, actions: [
          { action: 9, groupId: 0, lightIndex: 0, numberValue: 0, stringValue: '', r: 255, g: 255, b: 255 }, // SceneNext
          blankAutomationAction(), blankAutomationAction(),
        ] },
      blankAutomationRule(),
      blankAutomationRule(),
    ],
  },
];

// wifiConnected/ip/wifiAwaitingApConfirm are derived at request time in
// selfWithLights() from the module's wifiConnected SSID-tracking state below.
const MOCK_SELF  = { name: 'Mock Device',   mac: '11:22:33:44:55:66', online: true,  sceneSyncEnabled: true,  hasWifiNetworks: true,  wifiConnecting: false, channel: 6, channelSearching: false, version: '2026.06.27.0', fwState: 'checking', batteryPresent: true, batteryPercent: 82, batteryCharging: false };
const MOCK_PEERS = [
  { name: 'Mock Light 2', mac: '22:33:44:55:66:77', lights: [{ index: 0, name: 'Kitchen', groupId: 0 }], sound: { name: 'Kitchen Speaker', audioGroupId: 0, volumeOverrideEnabled: true, volume: 180 }, online: true,  rssi: -65, sceneSyncEnabled: true,  wifiConnected: true,  hasWifiNetworks: true,  wifiConnecting: false, version: '2026.01.01.0', fwState: 'idle', batteryPresent: true,  batteryPercent: 46, batteryCharging: false },
  { name: 'Mock Light 3', mac: '33:44:55:66:77:88', lights: [{ index: 0, name: 'Hallway', groupId: 1 }, { index: 1, name: 'Closet', groupId: 0 }], sound: null, online: true,  rssi: -80, sceneSyncEnabled: false, wifiConnected: false, hasWifiNetworks: true,  wifiConnecting: false, version: '2026.01.01.0', fwState: 'idle', batteryPresent: true,  batteryPercent: 12, batteryCharging: false },
  { name: 'Mock Light 4', mac: '44:55:66:77:88:99', lights: [{ index: 0, name: '', groupId: 0 }], sound: { name: 'Living Room Speaker', audioGroupId: 1, volumeOverrideEnabled: false, volume: 150 }, online: true,  rssi: -55, sceneSyncEnabled: true,  wifiConnected: true,  hasWifiNetworks: false, wifiConnecting: false, version: '2026.06.27.0', fwState: 'idle', batteryPresent: true,  batteryPercent: 97, batteryCharging: true  },
  { name: 'Mock Light 5', mac: '55:66:77:88:99:aa', lights: [{ index: 0, name: 'Garage', groupId: 0 }], sound: null, online: true,  rssi: -70, sceneSyncEnabled: true,  wifiConnected: false, hasWifiNetworks: true,  wifiConnecting: true,  version: '2026.01.01.0', fwState: 'idle', batteryPresent: false, batteryPercent: 0,  batteryCharging: false },
];

// Mirrors EventLog's ring buffer (src/events/EventLog.h, issue #442): the
// order GenericEvent broadcasts (matching mockAutomations' 'buzz.press'/
// 'buzz.reset' eventTypes) arrived at this device. Static sample data since
// nothing in the mock server generates live mesh traffic.
const mockEvents = [
  { name: 'Mock Light 2', eventType: 'buzz.press', payload: 1, order: 0 },
  { name: 'Mock Light 4', eventType: 'buzz.press', payload: 2, order: 1 },
  { name: 'Mock Device',  eventType: 'buzz.reset', payload: 0, order: 2 },
];
// Sent as a live 't':'event' WS push on connect (not part of mockEvents/
// GET /api/events) so a freshly loaded page also demonstrates the live-append
// path, without duplicating a row the initial GET already returned.
const mockLiveEvent = { name: 'Mock Light 3', eventType: 'buzz.press', payload: 3, order: 3 };

// Devices seen only via HelloMsg (different/incompatible firmware — see
// docs/mesh-compatibility.md and WebServer.h::_buildPeersJson's
// discoveredPeers array). Mirrors just what a real device's PeerInfo carries
// for a helloOnly entry: mac/name/version/online/wifiConnected/
// hasWifiNetworks, nothing else. Two entries so the dashboard's WiFi-status
// badge exercises both the "needs a config push first" and "could check for
// an update right now" cases.
const MOCK_DISCOVERED_PEERS = [
  { name: 'New Device', mac: '66:77:88:99:aa:bb', version: '2026.07.01.0', online: true, wifiConnected: false, hasWifiNetworks: false },
  { name: 'Almost Set Up', mac: '77:88:99:aa:bb:cc', version: '2026.06.15.0', online: true, wifiConnected: true, hasWifiNetworks: true },
];

const wifiNetworks = [
  { ssid: 'HomeNetwork', password: 'secret1' },
  { ssid: 'WorkWifi',    password: 'secret2' },
  { ssid: 'GuestWifi',   password: 'secret3' },
];
let wifiConnected = 'HomeNetwork';
const MOCK_SELF_IP = '192.168.1.87';
// Shortened well below the firmware's 5-minute kApConfirmHoldMs so the
// auto-timeout fallback is practical to exercise in local/CI testing.
const MOCK_AP_CONFIRM_TIMEOUT_MS = 8000;
let mockWifiAwaitingApConfirm = false;
let mockApConfirmTimer = null;

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
  return {
    ...MOCK_SELF,
    wifiConnected: wifiConnected !== null,
    ip: wifiConnected !== null ? MOCK_SELF_IP : '',
    wifiAwaitingApConfirm: mockWifiAwaitingApConfirm,
    lights: mockSelfLights(),
    sound: mockSelfSound(),
  };
}

// Broadcast current self+peers state to all connected WS clients.
function broadcastPeers() {
  const msg = JSON.stringify({
    t: 'peers',
    self: selfWithLights(),
    peers: MOCK_PEERS,
    discoveredPeers: MOCK_DISCOVERED_PEERS,
    wifiSingleClientMode: MOCK_CONFIG.wifiSingleClientMode,
  });
  wss.clients.forEach(c => { if (c.readyState === 1) c.send(msg); });
}

function broadcastAudioGroups() {
  const msg = JSON.stringify({ t: 'audioGroups', list: mockAudioGroups });
  wss.clients.forEach(c => { if (c.readyState === 1) c.send(msg); });
}

function broadcastEventsCleared() {
  const msg = JSON.stringify({ t: 'eventsCleared' });
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
  // secrets (mqttPassword, githubToken) via GET /api/config, and never
  // accepts prTrack from the client — it's device-local state, only ever
  // set by a successful PR install (see /api/update/apply-pr).
  const rest = { ...req.body };
  delete rest.mqttPassword;
  delete rest.githubToken;
  delete rest.prTrack;

  // Mirrors _postConfig's I2C bus guard: reject disabling it while a sound
  // output, button, or the configured expander still needs it. The expander
  // block below sees this already-resolved target chip, matching how the
  // real device applies the bus fields before the expander fields.
  const targetExpanderChip = 'expanderChip' in rest ? rest.expanderChip : MOCK_CONFIG.expanderChip;
  if ('i2cSdaPin' in rest && 'i2cSclPin' in rest) {
    const disablingBus = rest.i2cSdaPin === PIN_UNUSED || rest.i2cSclPin === PIN_UNUSED;
    if (disablingBus) {
      const busInUse = mockSounds.some(s => s.exists) || targetExpanderChip !== 0;
      if (busInUse) {
        return res.status(400).json({
          error: 'I2C bus still used by a configured sound output, button, or the configured expander',
        });
      }
    } else if (rest.i2cSdaPin === rest.i2cSclPin) {
      return res.status(400).json({ error: 'SDA and SCL must be different pins' });
    }
  }

  // Mirrors _postConfig's expander guard: enabling it requires the (possibly
  // just-updated-above) bus; disabling it requires nothing still using it.
  if ('expanderChip' in rest || 'expanderAddress' in rest) {
    const sda = 'i2cSdaPin' in rest ? rest.i2cSdaPin : MOCK_CONFIG.i2cSdaPin;
    const scl = 'i2cSclPin' in rest ? rest.i2cSclPin : MOCK_CONFIG.i2cSclPin;
    if (targetExpanderChip !== 0 && (sda === PIN_UNUSED || scl === PIN_UNUSED)) {
      return res.status(400).json({ error: 'configure the device I2C bus first' });
    }
    if (targetExpanderChip === 0) {
      const expanderUsed = mockSounds.some(s => s.exists && s.paViaExpander) ||
        mockButtons.some(b => b.exists && b.viaExpander);
      if (expanderUsed) {
        return res.status(400).json({ error: 'expander still used by a configured sound output or button' });
      }
    }
  }

  // Mirrors _postConfig's rebootNeeded logic: deviceName/otaPort/otaEnabled
  // are one-shot on the real device (mDNS/ArduinoOTA/AP SSID init at boot
  // only); the I2C bus and expander are too, since both are only ever
  // brought up once at boot (see main.cpp) with no live-reconfigure path.
  // Every other field applies live, no reboot.
  const rebooting = [
    'deviceName', 'otaPort', 'otaEnabled',
    'i2cSdaPin', 'i2cSclPin', 'expanderChip', 'expanderAddress',
  ].some(k => k in rest && rest[k] !== MOCK_CONFIG[k]);

  Object.assign(MOCK_CONFIG, rest);
  res.json({ ok: true, rebooting });
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

  // Mirrors WebServer.h::_addWifi kicking off a live connect attempt when
  // this device has no active WiFi connection, instead of waiting for a
  // reboot. Holds the mock "AP" open (awaitingApConfirm) until confirmed or
  // the shortened mock timeout elapses.
  if (wifiConnected === null) {
    setTimeout(() => {
      wifiConnected = ssid;
      mockWifiAwaitingApConfirm = true;
      broadcastPeers();
      clearTimeout(mockApConfirmTimer);
      mockApConfirmTimer = setTimeout(() => {
        mockWifiAwaitingApConfirm = false;
        broadcastPeers();
      }, MOCK_AP_CONFIRM_TIMEOUT_MS);
    }, 1500);
  }
});
app.post('/api/wifi/confirm-disable-ap', (_req, res) => {
  mockWifiAwaitingApConfirm = false;
  clearTimeout(mockApConfirmTimer);
  broadcastPeers();
  res.json({ ok: true });
});
app.post('/api/wifi/delete', (req, res) => {
  const { ssid } = req.body;
  if (!ssid) return res.status(400).json({ error: 'ssid required' });
  const idx = wifiNetworks.findIndex(n => n.ssid === ssid);
  if (idx !== -1) wifiNetworks.splice(idx, 1);
  if (wifiConnected === ssid) { wifiConnected = null; broadcastPeers(); }
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
  discoveredPeers: MOCK_DISCOVERED_PEERS,
  wifiSingleClientMode: MOCK_CONFIG.wifiSingleClientMode,
}));
app.get('/api/events', (req, res) => {
  const eventType = (req.query.eventType || '').toString();
  const filtered = eventType ? mockEvents.filter(e => e.eventType === eventType) : mockEvents;
  const limit = MOCK_CONFIG.eventLogLimit || 10;
  res.json({ events: filtered.slice(-limit) });
});
app.post('/api/events/clear', (_req, res) => {
  mockEvents.length = 0;
  res.json({ ok: true });
  broadcastEventsCleared();
});
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
app.post('/api/peers/setaudiogroup', (req, res) => {
  const { mac, audioGroupId } = req.body || {};
  // Mirrors /api/peers/setgroup: only self-mac assignments are persisted
  // locally; remote peers would relay over the mesh on real hardware.
  if (mac === MOCK_SELF.mac) {
    const sound = mockSounds.find(s => s.exists);
    if (sound && mockAudioGroups.find(g => g.id === audioGroupId)) sound.audioGroupId = audioGroupId;
    broadcastPeers();
  }
  res.json({ ok: true });
});
app.post('/api/peers/setvolume', (req, res) => {
  const { mac, volume, overrideEnabled } = req.body || {};
  if (mac === MOCK_SELF.mac) {
    const sound = mockSounds.find(s => s.exists);
    if (sound) {
      sound.volumeOverrideEnabled = Boolean(overrideEnabled);
      if (overrideEnabled) sound.volume = clampVolume(volume);
    }
    broadcastPeers();
  }
  res.json({ ok: true });
});
app.post('/api/peers/setscenesync', (_req, res) => res.json({ ok: true }));

// Simulates a discovered (hello-only) device reconnecting to WiFi some time
// after receiving a config push, so the single-button "Set up device" flow
// (data/index.html's setupDiscoveredDevice) can be exercised end to end
// against the mock server without real hardware.
function simulateDiscoveredWifiConnect(peer) {
  setTimeout(() => {
    peer.wifiConnected = true;
    broadcastPeers();
  }, 2000);
}

// Simulates a discovered device successfully installing an update and
// rejoining as a full compatible peer — mirrors what a real device does
// once Updater::triggerAsync() finds+installs an update and reboots onto
// firmware whose PresenceMsg this mesh accepts again.
function simulateDiscoveredGraduate(peer) {
  setTimeout(() => {
    const idx = MOCK_DISCOVERED_PEERS.indexOf(peer);
    if (idx === -1) return;
    MOCK_DISCOVERED_PEERS.splice(idx, 1);
    MOCK_PEERS.push({
      name: peer.name, mac: peer.mac, lights: [], sound: null, online: true, rssi: -60,
      sceneSyncEnabled: true, wifiConnected: true, hasWifiNetworks: true, wifiConnecting: false,
      version: MOCK_SELF.version, fwState: 'idle',
      batteryPresent: false, batteryPercent: 0, batteryCharging: false,
    });
    broadcastPeers();
  }, 3000);
}

app.post('/api/peers/pushconfig', (req, res) => {
  res.json({ ok: true });
  const { mac } = req.body || {};
  const discovered = MOCK_DISCOVERED_PEERS.find(p => p.mac === mac);
  if (discovered) simulateDiscoveredWifiConnect(discovered);
});
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
  const discovered = MOCK_DISCOVERED_PEERS.find(p => p.mac === mac);
  if (discovered) simulateDiscoveredGraduate(discovered);
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

// dataPin is always active; clockPin only counts for WS2801 (ledType 1) —
// WS2812B is single-wire, so a default/leftover clockPin value there is
// never actually driven (see Config::isPinInUse in Config.cpp).
function lightPins(l) {
  const pins = [l.dataPin];
  if (l.ledType === 1) pins.push(l.clockPin);
  return pins;
}

// Mirrors WebServer.h::_lightPinConflict: every active pin on `l` must be
// mutually distinct and not already claimed by a light, button, sound
// output, or the device I2C bus. Pass l.index as excludeIndex when
// validating an update. A button only counts as a native-GPIO conflict when
// it isn't expander-backed — see Config::isPinInUse in Config.cpp.
function lightPinConflict(l, excludeIndex) {
  const pins = lightPins(l);
  if (new Set(pins).size !== pins.length) return 'duplicate pin within light config';
  if (pins.some(p => busPins().includes(p))) return 'pin already in use';
  if (mockLights.some(other => other.index !== excludeIndex && lightPins(other).some(p => pins.includes(p))))
    return 'pin already in use';
  if (mockButtons.some(b => !b.viaExpander && pins.includes(b.pin))) return 'pin already in use';
  if (mockSounds.some(s => soundPins(s).some(p => pins.includes(p)))) return 'pin already in use';
  return null;
}

app.get('/api/lights', (_req, res) => res.json({ lights: mockLights, maxLights: 4 }));
app.post('/api/lights/add', (req, res) => {
  const free = [0,1,2,3].find(i => !mockLights.find(l => l.index === i));
  if (free === undefined) return res.status(400).json({ error: 'light limit reached' });
  const { name = '', ledType = 0, colorOrder, dataPin = 13, clockPin = 14, width = 1, height = 1, matrixStart = 0, matrixDir = 0, matrixSerpentine = false, wrapWidth = false, wrapHeight = false, groupId = 0, brightnessLimit = 255, brightnessScale = 255 } = req.body || {};
  // Mirrors Config.h::defaultColorOrder: WS2812B (0) is conventionally wired GRB (2), WS2801 (1) RGB (0).
  const resolvedColorOrder = colorOrder !== undefined ? colorOrder : (ledType === 0 ? 2 : 0);
  const light = { index: free, name, ledType, colorOrder: resolvedColorOrder, dataPin, clockPin, width, height, matrixStart, matrixDir, matrixSerpentine, wrapWidth, wrapHeight, groupId, brightnessLimit, brightnessScale };
  const conflict = lightPinConflict(light, free);
  if (conflict) return res.status(400).json({ error: conflict });
  mockLights.push(light);
  res.json({ ok: true, index: free });
});
app.post('/api/lights/update', (req, res) => {
  const { index, ...fields } = req.body || {};
  const light = mockLights.find(l => l.index === index);
  if (!light) return res.status(404).json({ error: 'not found' });
  const pinsChanged = ['ledType', 'dataPin', 'clockPin'].some(k => k in fields);
  if (pinsChanged) {
    const candidate = { ...light, ...fields };
    const conflict = lightPinConflict(candidate, index);
    if (conflict) return res.status(400).json({ error: conflict });
  }
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

// True once MOCK_CONFIG.i2cSdaPin/i2cSclPin are both set — mirrors
// WebServer.h::_i2cBusConfigured(), required before a sound output can be
// added, or before anything can be routed through the expander.
function i2cBusConfigured() {
  return MOCK_CONFIG.i2cSdaPin !== PIN_UNUSED && MOCK_CONFIG.i2cSclPin !== PIN_UNUSED;
}
function busPins() {
  return [MOCK_CONFIG.i2cSdaPin, MOCK_CONFIG.i2cSclPin].filter(p => p !== PIN_UNUSED);
}
// True once MOCK_CONFIG.expanderChip is set — mirrors
// WebServer.h::_expanderConfigured(), required before a sound's PA-enable
// pin or a button can be routed through the device's single expander.
function expanderConfigured() {
  return MOCK_CONFIG.expanderChip !== 0;
}

// paEnablePin only occupies the ESP32 GPIO address space when paViaExpander
// is false — when true it's a pin index in the device expander's separate
// address space (see Config.h::IoExpanderChip) and must not be
// cross-checked against real GPIOs.
function soundPins(s) {
  const pins = [s.i2sMclkPin, s.i2sBclkPin, s.i2sWsPin, s.i2sDoutPin];
  if (!s.paViaExpander) pins.push(s.paEnablePin);
  return pins.filter(p => p !== PIN_UNUSED);
}

// Mirrors Config.cpp::isExpanderPinInUse: true if pin index `pin` (0-15) on
// the device's single expander is already claimed by another button, or by
// the sound's PA-enable pin. Pass excludeSoundPa=true when validating the
// sound output's own PA pin, so an unchanged value doesn't collide with
// itself (there's only ever one sound).
function isExpanderPinInUse(pin, excludeButtonIndex, excludeSoundPa = false) {
  if (mockButtons.some(b => b.index !== excludeButtonIndex && b.viaExpander && b.pin === pin))
    return true;
  if (excludeSoundPa) return false;
  return mockSounds.some(s => s.paViaExpander && s.paEnablePin === pin);
}

// Mirrors Config.cpp::isPinInUse for a native-GPIO button/light/sound pin —
// also checks the device-wide I2C bus pins (MOCK_CONFIG.i2cSdaPin/i2cSclPin).
function isButtonPinInUse(pin, excludeIndex) {
  if (busPins().includes(pin)) return true;
  if (mockLights.some(l => l.dataPin === pin || l.clockPin === pin)) return true;
  if (mockSounds.some(s => soundPins(s).includes(pin))) return true;
  return mockButtons.some(b => b.index !== excludeIndex && !b.viaExpander && b.pin === pin);
}

// Mirrors WebServer.h::_buttonPinConflict: routes to the native-GPIO or
// expander address space depending on b.viaExpander.
function buttonPinConflict(b, excludeIndex) {
  if (b.viaExpander) {
    if (!expanderConfigured()) return 'configure the device I2C expander first';
    if (isExpanderPinInUse(b.pin, excludeIndex)) return 'expander pin already in use';
    return null;
  }
  if (isButtonPinInUse(b.pin, excludeIndex)) return 'pin already in use';
  return null;
}

// Mirrors WebServer.h::_soundPinConflict: every non-unused pin on `s` must be
// mutually distinct and not already claimed by a light, button, other sound
// output, or the device I2C bus. Pass s.index as excludeIndex when validating
// an update.
function soundPinConflict(s, excludeIndex) {
  const pins = soundPins(s);
  if (new Set(pins).size !== pins.length) return 'duplicate pin within sound config';
  if (pins.some(p => busPins().includes(p))) return 'pin already in use';
  if (mockLights.some(l => pins.includes(l.dataPin) || pins.includes(l.clockPin))) return 'pin already in use';
  if (mockButtons.some(b => !b.viaExpander && pins.includes(b.pin))) return 'pin already in use';
  if (mockSounds.some(other => other.index !== excludeIndex && soundPins(other).some(p => pins.includes(p))))
    return 'pin already in use';
  if (s.paViaExpander && s.paEnablePin !== PIN_UNUSED) {
    if (!expanderConfigured()) return 'configure the device I2C expander first';
    if (isExpanderPinInUse(s.paEnablePin, undefined, /*excludeSoundPa=*/true))
      return 'expander pin already in use';
  }
  return null;
}

const MAX_SOUNDS = 1;

app.get('/api/sounds', (_req, res) => res.json({ sounds: mockSounds, maxSounds: MAX_SOUNDS }));
app.post('/api/sounds/add', (req, res) => {
  if (!i2cBusConfigured()) return res.status(400).json({ error: 'configure the device I2C bus in Hardware settings first' });
  const free = Array.from({ length: MAX_SOUNDS }, (_, i) => i).find(i => !mockSounds.find(s => s.index === i));
  if (free === undefined) return res.status(400).json({ error: 'sound limit reached' });
  const {
    name = '', chip = 0,
    i2cAddress = 0x18, i2sMclkPin = PIN_UNUSED, i2sBclkPin = PIN_UNUSED,
    i2sWsPin = PIN_UNUSED, i2sDoutPin = PIN_UNUSED, paEnablePin = PIN_UNUSED,
    paEnableActiveHigh = true, paViaExpander = false,
  } = req.body || {};
  if ([i2sBclkPin, i2sWsPin, i2sDoutPin].includes(PIN_UNUSED)) {
    return res.status(400).json({ error: 'missing required pin' });
  }
  const sound = { index: free, name, chip, i2cAddress, i2sMclkPin, i2sBclkPin, i2sWsPin, i2sDoutPin, paEnablePin, paEnableActiveHigh, paViaExpander, exists: true };
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
  // audioGroupId/volume aren't hardware config (no reboot needed) — applied
  // live, same as WebServer.h::_updateSound. They're also settable
  // cross-device via /api/peers/setaudiogroup and /api/peers/setvolume below.
  const hwChanged = Object.keys(fields).some(k => k !== 'name' && k !== 'audioGroupId' && k !== 'volume');
  if (hwChanged) {
    if ([candidate.i2sBclkPin, candidate.i2sWsPin, candidate.i2sDoutPin].includes(PIN_UNUSED)) {
      return res.status(400).json({ error: 'missing required pin' });
    }
    const conflict = soundPinConflict(candidate, index);
    if (conflict) return res.status(400).json({ error: conflict });
  }
  if ('volume' in fields) fields.volume = clampVolume(fields.volume);
  Object.assign(sound, fields);
  broadcastPeers();
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

// ── Audio groups ─────────────────────────────────────────────────────────────
// Mirrors WebServer.h's audiogroups handlers — same shape as the light-group
// endpoints above, minus any per-group payload (see AudioGroupConfig).
const MAX_AUDIO_GROUPS = 8; // mirrors Config::MAX_AUDIO_GROUPS; id 0 is reserved for Default

app.get('/api/audiogroups', (_req, res) => res.json({ audioGroups: mockAudioGroups }));
app.post('/api/audiogroups/create', (req, res) => {
  const free = Array.from({ length: MAX_AUDIO_GROUPS - 1 }, (_, i) => i + 1)
    .find(i => !mockAudioGroups.find(g => g.id === i));
  if (free === undefined) return res.status(400).json({ error: 'group limit reached' });
  const { name = 'New Group' } = req.body || {};
  mockAudioGroups.push({ id: free, name, exists: true, volume: SOUND_VOLUME_MAX });
  res.json({ ok: true, id: free });
  broadcastAudioGroups();
});
app.post('/api/audiogroups/update', (req, res) => {
  const { id, ...fields } = req.body || {};
  const group = mockAudioGroups.find(g => g.id === id);
  if (!group) return res.status(404).json({ error: 'not found' });
  if ('volume' in fields) fields.volume = clampVolume(fields.volume);
  Object.assign(group, fields);
  res.json({ ok: true });
  broadcastAudioGroups();
});
app.post('/api/audiogroups/delete', (req, res) => {
  const { id } = req.body || {};
  if (id === 0) return res.status(400).json({ error: 'cannot delete Default' });
  const idx = mockAudioGroups.findIndex(g => g.id === id);
  if (idx === -1) return res.status(404).json({ error: 'not found' });
  mockAudioGroups.splice(idx, 1);
  for (const s of mockSounds) if (s.audioGroupId === id) s.audioGroupId = 0;
  res.json({ ok: true });
  broadcastAudioGroups();
});

// ── Playlists ────────────────────────────────────────────────────────────────
// Mirrors WebServer.h's playlist handlers / PlaylistManager — id/name/loop/files.
app.get('/api/playlists', (_req, res) => res.json({ playlists: [...mockPlaylists.values()] }));
app.post('/api/playlists/create', (req, res) => {
  const name = String((req.body || {}).name || 'Unnamed');
  const id = `mockpl-${Date.now().toString(36)}`;
  mockPlaylists.set(id, { id, name, loop: false, files: [] });
  res.json({ ok: true, id });
});
app.post('/api/playlists/save', (req, res) => {
  const { id } = req.body || {};
  if (!id) return res.status(400).json({ error: 'missing or invalid id' });
  mockPlaylists.set(id, { ...req.body });
  res.json({ ok: true });
});
app.post('/api/playlists/delete', (req, res) => {
  const { id } = req.body || {};
  if (!id || !mockPlaylists.has(id)) return res.status(404).json({ error: 'not found' });
  mockPlaylists.delete(id);
  res.json({ ok: true });
});

// ── Playback triggers ────────────────────────────────────────────────────────
// Fire-and-forget, same as the real device — no state to report back (see the
// "no cross-device playback state" design decision). Logged to the console
// only, since this mock has no real mesh/speaker to actually play anything.
app.post('/api/audio/play/file', (req, res) => {
  const { audioGroupId, filename, loop } = req.body || {};
  if (!filename) return res.status(400).json({ error: 'missing filename' });
  if (!mockAudioGroups.find(g => g.id === audioGroupId)) return res.status(404).json({ error: 'not found' });
  console.log(`[mock audio] play file "${filename}" on group ${audioGroupId} loop=${!!loop}`);
  res.json({ ok: true });
});
app.post('/api/audio/play/playlist', (req, res) => {
  const { audioGroupId, playlistId } = req.body || {};
  if (!playlistId) return res.status(400).json({ error: 'missing playlistId' });
  if (!mockAudioGroups.find(g => g.id === audioGroupId)) return res.status(404).json({ error: 'not found' });
  console.log(`[mock audio] play playlist "${playlistId}" on group ${audioGroupId}`);
  res.json({ ok: true });
});
app.post('/api/audio/stop', (req, res) => {
  const { audioGroupId } = req.body || {};
  console.log(`[mock audio] stop group ${audioGroupId}`);
  res.json({ ok: true });
});

app.get('/api/buttons', (_req, res) => res.json({ buttons: mockButtons, maxButtons: 4 }));
app.post('/api/buttons/add', (req, res) => {
  const free = [0,1,2,3].find(i => !mockButtons.find(b => b.index === i));
  if (free === undefined) return res.status(400).json({ error: 'button limit reached' });
  const { name = '', pin = 0, activeLow = true, viaExpander = false, onShortPress, onLongPress, onDoubleClick } = req.body || {};
  const conflict = buttonPinConflict({ pin, viaExpander }, free);
  if (conflict) return res.status(400).json({ error: conflict });
  const blankAction = () => ({ action: 0, groupId: 0, lightIndex: 0, numberValue: 0, stringValue: '', r: 255, g: 255, b: 255 });
  mockButtons.push({
    index: free, name, pin, activeLow, viaExpander, exists: true,
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
  const pinChanged = ['pin', 'viaExpander'].some(k => fields[k] !== undefined);
  if (pinChanged) {
    const candidate = { ...button, ...fields };
    const conflict = buttonPinConflict(candidate, index);
    if (conflict) return res.status(400).json({ error: conflict });
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

app.get('/api/automations', (_req, res) => res.json({
  automations: mockAutomations,
  maxAutomations: MAX_AUTOMATION_BINDINGS,
  maxRulesPerBinding: MAX_RULES_PER_BINDING,
  maxActionsPerRule: MAX_ACTIONS_PER_RULE,
}));
app.post('/api/automations/add', (req, res) => {
  const free = Array.from({ length: MAX_AUTOMATION_BINDINGS }, (_, i) => i).find(i => !mockAutomations.find(a => a.index === i));
  if (free === undefined) return res.status(400).json({ error: 'automation limit reached' });
  const { triggerType = 0, eventType = '', rules } = req.body || {};
  if (!eventType) return res.status(400).json({ error: 'eventType required' });
  const filledRules = Array.from({ length: MAX_RULES_PER_BINDING }, (_, i) => (rules && rules[i]) || blankAutomationRule());
  mockAutomations.push({ index: free, triggerType, eventType, exists: true, rules: filledRules });
  res.json({ ok: true, index: free });
});
app.post('/api/automations/update', (req, res) => {
  const { index, ...fields } = req.body || {};
  const automation = mockAutomations.find(a => a.index === index);
  if (!automation) return res.status(404).json({ error: 'not found' });
  if (fields.eventType !== undefined && !fields.eventType) return res.status(400).json({ error: 'eventType required' });
  Object.assign(automation, fields);
  res.json({ ok: true });
});
app.post('/api/automations/delete', (req, res) => {
  const { index } = req.body || {};
  const idx = mockAutomations.findIndex(a => a.index === index);
  if (idx === -1) return res.status(404).json({ error: 'not found' });
  mockAutomations.splice(idx, 1);
  res.json({ ok: true });
});

// Mirrors SdCardManager: auto-detected hardware, no add/edit config, just
// status + a flat file list. hwSupported mirrors SdCardManager::kHwSupported
// (true only on an esp32s3 build with SD_CARD_*_PIN defined); present mirrors
// whether a card actually responded at boot.
const mockStorage = {
  hwSupported: true,
  present: true,
  totalBytes: 15931539456, // ~14.8 GB, a typical microSD reporting
  files: [
    { name: 'doorbell.wav', size: 245760 },
    { name: 'alarm.wav', size: 1048576 },
    // A pre-existing, non-.wav file — exercises GET /api/storage filtering
    // it out of the list (see WebServer.h::_getStorage): a real card can
    // easily carry one of these (a prior recording, OS metadata from
    // formatting on a computer, ...) and it must never be listed, since
    // upload/delete would 400 "invalid filename" on it.
    { name: 'IMG_0001.JPG', size: 3145728 },
  ],
};

// Mirrors WebServer.h::_isValidWavName: bare filename (no path separators),
// non-empty, ending in ".wav" (case-insensitive).
function isValidWavName(name) {
  return typeof name === 'string' && name.length > 4 &&
    !name.includes('/') && !name.includes('\\') && /\.wav$/i.test(name);
}

function storageWavFiles() {
  return mockStorage.files.filter(f => isValidWavName(f.name));
}

function storageUsedBytes() {
  return mockStorage.files.reduce((sum, f) => sum + f.size, 0);
}

app.get('/api/storage', (_req, res) => res.json({
  hwSupported: mockStorage.hwSupported,
  present: mockStorage.present,
  totalBytes: mockStorage.present ? mockStorage.totalBytes : 0,
  usedBytes: mockStorage.present ? storageUsedBytes() : 0,
  files: mockStorage.present ? storageWavFiles() : [],
}));

app.post('/api/storage/upload', express.raw({ type: 'application/octet-stream', limit: '64mb' }), (req, res) => {
  const name = req.query.name;
  if (!isValidWavName(name)) return res.status(400).json({ error: 'invalid filename' });
  if (!mockStorage.present) return res.status(400).json({ error: 'no SD card' });
  const size = Buffer.isBuffer(req.body) ? req.body.length : 0;
  const existing = mockStorage.files.find(f => f.name === name);
  if (existing) existing.size = size;
  else mockStorage.files.push({ name, size });
  res.json({ ok: true });
});

app.post('/api/storage/delete', (req, res) => {
  const { name } = req.body || {};
  if (!isValidWavName(name)) return res.status(400).json({ error: 'invalid filename' });
  const idx = mockStorage.files.findIndex(f => f.name === name);
  if (idx === -1) return res.status(404).json({ error: 'not found' });
  mockStorage.files.splice(idx, 1);
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
    brightness: 255, speed: 1, seq: 0, syncEnabled: true, proximityScale: 1.0,
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

// Drives mockUpdate through downloading -> done -> simulateReboot, shared by
// /api/update/apply, /api/update/trigger, and /api/update/apply-pr. onDone (if
// given) runs right as flashing completes, before the reboot simulation starts.
function _simulateFlash(newVersion, onDone) {
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
      if (onDone) onDone();
      simulateReboot(newVersion);
    }
  }, 500);
}

app.post('/api/update/apply', (_req, res) => {
  if (!mockUpdate.hasUpdate) {
    return res.status(400).json({ error: 'no update available' });
  }
  res.json({ ok: true });
  _simulateFlash(mockUpdate.latestVersion);
});

app.post('/api/update/trigger', (_req, res) => {
  res.json({ ok: true });
  if (mockUpdate.hasUpdate) {
    _simulateFlash(mockUpdate.latestVersion);
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

// Mirrors Updater::listPrBuildsAsync()/applyPrAsync() — every endpoint below
// requires prOtaEnabled, mirroring WebServer.h's 403 gating.
const MOCK_OPEN_PRS = [
  { number: 391, title: 'web: add scene editor', tag: 'pr-391' },
  { number: 388, title: 'mqtt: handle reconnect on timeout', tag: 'pr-388' },
];
const mockPrList = { state: 'idle', prs: [], error: null };

app.post('/api/update/prs/refresh', (_req, res) => {
  if (!MOCK_CONFIG.prOtaEnabled) return res.status(403).json({ error: 'PR installs are disabled' });
  mockPrList.state = 'loading';
  mockPrList.error = null;
  res.json({ ok: true });
  setTimeout(() => {
    mockPrList.state = 'done';
    mockPrList.prs   = MOCK_OPEN_PRS;
  }, 800);
});

app.get('/api/update/prs', (_req, res) => {
  const out = { state: mockPrList.state, prs: mockPrList.prs };
  if (mockPrList.error) out.error = mockPrList.error;
  res.json(out);
});

app.post('/api/update/apply-pr', (req, res) => {
  if (!MOCK_CONFIG.prOtaEnabled) return res.status(403).json({ error: 'PR installs are disabled' });
  const tag = (req.body || {}).tag || '';
  res.json({ ok: true });
  const newVersion = tag ? `${tag}-dev` : '9999.0.0.0';
  _simulateFlash(newVersion, () => { MOCK_CONFIG.prTrack = tag; });
});

// ── Graphs (mock-only, issue #464) ──────────────────────────────────────────
// The dock-editor shell and its /api/graphs storage exist only in this mock
// for now — the real endpoints, validation, and execution arrive with the M2
// graph engine (GraphManager, SceneManager-style). Schema v1 per the rework
// plan; col/row on nodes and the notes array are editor-only metadata. The
// editor page lives in server/ (not data/) so it never ships to devices —
// LittleFS is nearly full (#377); it moves into the device UI with M8.
const GRAPH_NAME_RE = /^[a-z0-9-]{1,32}$/i;

const mockGraphs = new Map([
  ['buzzergame', {
    v: 1, name: 'buzzergame', active: true, requires: ['button:main', 'stage:main'],
    nodes: [
      { id: 1, type: 'button', role: 'button:main', col: 1, row: 1, cfg: {} },
      { id: 2, type: 'wait', col: 2, row: 1, cfg: { ms: 2000 } },
      { id: 3, type: 'action', col: 3, row: 1, cfg: { action: 'SceneNext' } },
    ],
    edges: [[1, 'pressed', 2, 'start'], [2, 'done', 3, 'trigger']],
    notes: [{ col: 1, row: 5, text: 'registration' }],
  }],
  ['nightlight', {
    v: 1, name: 'nightlight', active: false, requires: [],
    nodes: [
      { id: 1, type: 'mesh-receive', col: 1, row: 2, cfg: { eventType: 'buzz.press' } },
      { id: 2, type: 'range', col: 2, row: 2, cfg: { min: 1, max: 3 } },
      { id: 3, type: 'log', col: 3, row: 1, cfg: {} },
      { id: 4, type: 'action', col: 3, row: 3, cfg: { action: 'ColorSet' } },
    ],
    edges: [[1, 'payload', 2, 'value'], [2, 'inRange', 4, 'trigger'], [2, 'outOfRange', 3, 'value']],
    notes: [],
  }],
]);

app.get('/graphs.html', (_req, res) =>
  res.sendFile(join(dirname(fileURLToPath(import.meta.url)), 'graphs.html')));

app.get('/api/graphs', (_req, res) => res.json({
  graphs: [...mockGraphs.values()].map(({ name, active }) => ({ name, active })),
}));

app.get('/api/graphs/:name', (req, res) => {
  const graph = mockGraphs.get(req.params.name);
  if (!graph) return res.status(404).json({ error: 'not found' });
  res.json(graph);
});

// Structural stub validation only — the authoritative validate lives in the
// M2 engine's load/validate/compile.
app.put('/api/graphs/:name', (req, res) => {
  const name = req.params.name;
  if (!GRAPH_NAME_RE.test(name)) return res.status(400).json({ error: 'invalid name' });
  const doc = req.body || {};
  if (doc.v !== 1 || doc.name !== name || !Array.isArray(doc.nodes) || !Array.isArray(doc.edges)) {
    return res.status(400).json({ error: 'invalid graph document' });
  }
  mockGraphs.set(name, doc);
  res.json({ ok: true });
});

app.delete('/api/graphs/:name', (req, res) => {
  if (!mockGraphs.delete(req.params.name)) return res.status(404).json({ error: 'not found' });
  res.json({ ok: true });
});

app.get('/*path', (_req, res) => res.sendFile(join(DATA_DIR, 'index.html')));

const server = http.createServer(app);

const wss = new WebSocketServer({ server, path: '/ws' });
wss.on('connection', ws => {
  if (_rebooting) { ws.terminate(); return; }
  const send = data => ws.send(JSON.stringify(data));
  send({ t: 'log', l: 'I', m: 'Mock server connected' });
  send({ t: 'log', l: 'I', m: 'This is a development mock — no hardware attached' });
  send({ t: 'peers', self: selfWithLights(), peers: MOCK_PEERS, discoveredPeers: MOCK_DISCOVERED_PEERS, wifiSingleClientMode: MOCK_CONFIG.wifiSingleClientMode });
  send({ t: 'groups', list: MOCK_CONFIG.groups });
  send({ t: 'audioGroups', list: mockAudioGroups });
  send({ t: 'event', ...mockLiveEvent });
});

const PORT = process.env.PORT || 8080;
server.listen(PORT, () => console.log(`LightWitch mock server: http://localhost:${PORT}`));

export { app, server };
