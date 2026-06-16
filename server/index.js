import http from 'http';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import express from 'express';
import { WebSocketServer } from 'ws';
import { authRouter, requireAuth } from './auth.js';

const DATA_DIR = join(dirname(fileURLToPath(import.meta.url)), '..', 'data');

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

const MOCK_SELF  = { name: 'Mock Device',   mac: '11:22:33:44:55:66', groupId: 0, online: true };
const MOCK_PEERS = [{ name: 'Mock Light 2', mac: '22:33:44:55:66:77', groupId: 0, online: true, rssi: -65 }];

const scenes = new Map();

const app = express();
app.use(express.json());
app.use('/auth', authRouter);
app.use(requireAuth);
app.use(express.static(DATA_DIR));

app.get('/api/config', (_req, res) => res.json(MOCK_CONFIG));
app.post('/api/config', (_req, res) => res.json({ ok: true }));

app.get('/api/peers', (_req, res) => res.json({ self: MOCK_SELF, peers: MOCK_PEERS }));
app.post('/api/peers/setgroup', (_req, res) => res.json({ ok: true }));

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
  scenes.set(id, { id, name, w, h, fc: 0, frames: [] });
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

app.post('/api/groups/create',  (_req, res) => res.json({ ok: true }));
app.post('/api/groups/update',  (_req, res) => res.json({ ok: true }));
app.post('/api/groups/delete',  (_req, res) => res.json({ ok: true }));
app.post('/api/reset',          (_req, res) => res.json({ ok: true }));

app.get('*', (_req, res) => res.sendFile(join(DATA_DIR, 'index.html')));

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
