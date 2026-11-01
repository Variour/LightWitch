import http from 'node:http';
import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { WebSocketServer } from 'ws';
import { validateInput, errorResult } from './validate.js';

const PUBLIC_DIR = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', 'public');
const STATIC = {
  '/': ['index.html', 'text/html; charset=utf-8'],
  '/screen.css': ['screen.css', 'text/css; charset=utf-8'],
  '/screen.js': ['screen.js', 'text/javascript; charset=utf-8'],
};
const BODY_MAX = 256 * 1024;
const PUSH_MIN_MS = 80;
const PUSH_EVERY_MS = 1000;

// The local HTTP server: the screen (static page plus a WebSocket with the
// live state) and the JSON API for the CLI and the skill. Listens on
// 127.0.0.1 only and accepts only requests addressed to that host, so pages
// from elsewhere cannot reach it through the browser.
export function startServer({ game, port, host = '127.0.0.1' }) {
  const allowedHosts = new Set();

  const hostOk = (req) => allowedHosts.has((req.headers.host || '').toLowerCase());

  const send = (res, status, body, type = 'application/json; charset=utf-8') => {
    res.writeHead(status, { 'Content-Type': type, 'Cache-Control': 'no-store' });
    res.end(typeof body === 'string' || Buffer.isBuffer(body) ? body : JSON.stringify(body));
  };

  const readJson = (req) => new Promise((resolve) => {
    let size = 0;
    const chunks = [];
    req.on('data', (c) => {
      size += c.length;
      if (size <= BODY_MAX) chunks.push(c);
    });
    req.on('end', () => {
      if (size > BODY_MAX) return resolve({ ok: false, status: 413, body: errorResult([{ path: '(Datei)', message: 'Die Datei ist größer als 256 KB.' }]) });
      const text = Buffer.concat(chunks).toString('utf8').replace(/^\uFEFF/, '');
      try {
        return resolve({ ok: true, value: JSON.parse(text) });
      } catch (e) {
        return resolve({ ok: false, status: 400, body: errorResult([{ path: '(Datei)', message: `Kein gültiges JSON: ${e.message}` }]) });
      }
    });
  });

  const server = http.createServer(async (req, res) => {
    if (!hostOk(req)) return send(res, 403, { ok: false, error: 'Nur über 127.0.0.1 erreichbar.' });
    const url = new URL(req.url, 'http://localhost');
    try {
      if (req.method === 'GET' && STATIC[url.pathname]) {
        const [file, type] = STATIC[url.pathname];
        return send(res, 200, await fs.readFile(path.join(PUBLIC_DIR, file)), type);
      }
      if (req.method === 'GET' && url.pathname === '/api/state') return send(res, 200, game.snapshot());
      if (req.method === 'POST' && (url.pathname === '/api/load' || url.pathname === '/api/action')) {
        if (!(req.headers['content-type'] || '').startsWith('application/json')) {
          return send(res, 415, { ok: false, error: 'Content-Type muss application/json sein.' });
        }
        const body = await readJson(req);
        if (!body.ok) return send(res, body.status, body.body);
        if (url.pathname === '/api/load') {
          const v = validateInput(body.value);
          if (!v.ok) {
            game.emit('rejected', { source: 'HTTP', result: errorResult(v.errors) });
            return send(res, 422, errorResult(v.errors));
          }
          return send(res, 200, game.load({ kind: v.kind, value: v.value }));
        }
        const { action, ...args } = body.value || {};
        const result = await game.act(String(action), args);
        return send(res, result.ok ? 200 : 409, result);
      }
      return send(res, 404, { ok: false, error: 'Nicht gefunden.' });
    } catch (err) {
      return send(res, 500, { ok: false, error: err.message });
    }
  });

  const wss = new WebSocketServer({ noServer: true });
  server.on('upgrade', (req, socket, head) => {
    if (!hostOk(req) || new URL(req.url, 'http://localhost').pathname !== '/ws') {
      socket.destroy();
      return;
    }
    wss.handleUpgrade(req, socket, head, (ws) => {
      ws.send(JSON.stringify(game.snapshot()));
    });
  });

  let lastPush = 0;
  let pushTimer = null;
  const push = () => {
    pushTimer = null;
    lastPush = Date.now();
    if (!wss.clients.size) return;
    const text = JSON.stringify(game.snapshot());
    for (const ws of wss.clients) if (ws.readyState === 1) ws.send(text);
  };
  const schedule = () => {
    if (pushTimer) return;
    pushTimer = setTimeout(push, Math.max(0, lastPush + PUSH_MIN_MS - Date.now()));
  };
  game.on('change', schedule);
  const tick = setInterval(schedule, PUSH_EVERY_MS);

  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, host, () => {
      const actual = server.address().port;
      for (const h of ['127.0.0.1', 'localhost', '[::1]']) allowedHosts.add(`${h}:${actual}`);
      resolve({
        port: actual,
        url: `http://127.0.0.1:${actual}/`,
        close: () => new Promise((done) => {
          clearInterval(tick);
          clearTimeout(pushTimer);
          game.off('change', schedule);
          for (const ws of wss.clients) ws.terminate();
          wss.close();
          server.close(() => done());
          server.closeAllConnections?.();
        }),
      });
    });
  });
}
