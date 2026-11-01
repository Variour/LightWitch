#!/usr/bin/env node
// game-console: the laptop side of the LightWitch quiz buzzers.
// Without a command it runs the console; every other command talks to the
// running console over http://127.0.0.1:<port>/.

import fs from 'node:fs/promises';
import { parseArgs } from 'node:util';
import { startConsole, DEFAULT_HTTP_PORT, DEFAULT_INBOX } from '../src/app.js';

const HELP = `game-console - Konsole für die LightWitch-Quiz-Buzzer

Konsole starten:
  game-console [--port <pfad>] [--channel 1-13] [--fleet 0-65535]
               [--manual-admit] [--http-port ${DEFAULT_HTTP_PORT}] [--inbox <ordner>] [--trace]
  game-console --simulate 50 [--sim-loss 5] [--sim-latency 30] [--sim-reboots]

  --port <pfad>     Serieller Port des Gateways (COM5, /dev/cu.usbmodem1101,
                    /dev/ttyACM0). Ohne: der einzige Espressif-Port (USB 303a).
  --simulate <N>    Kein USB: eingebautes Gateway mit N simulierten Buzzern.
  --sim-loss <%>    Simulator: Funkverlust in Prozent je Sendung.
  --sim-latency <ms> Simulator: Funkverzögerung bis zu so vielen ms.
  --sim-reboots     Simulator: alle 30-60 s startet ein Buzzer neu.
  --channel <n>     Funkkanal der Sitzung (Standard 6).
  --fleet <n>       Flotte der Einladung (Standard 0 = alle Buzzer).
  --manual-admit    Geräte nicht automatisch aufnehmen.
  --http-port <n>   Port für Bildschirm und API (Standard ${DEFAULT_HTTP_PORT}).
  --inbox <ordner>  Eingangsordner für Runden (Standard ${DEFAULT_INBOX}).
  --trace           Jede Protokollzeile im Terminal mitschreiben.

An die laufende Konsole:
  game-console send <datei.json>      Runde oder Sitzung laden
  game-console round --file <datei>   dasselbe
  game-console next                   Weiter (der nächste Schritt)
  game-console session-start | login-open | login-close | end
  game-console announce [id] | open | close | reveal | idle | winner <n>
  game-console admit <mac|all> [n] | renumber <n> <neu> | release <n>
  game-console admit-mode auto|manual | usb-number [n|none]
  game-console status                 Kurzer Zustand
  game-console ports                  Serielle Ports auflisten
`;

const ACTIONS = new Set([
  'next', 'session-start', 'login-open', 'login-close', 'announce', 'open', 'close', 'reveal', 'idle',
  'winner', 'admit', 'renumber', 'release', 'end', 'admit-mode', 'usb-number',
]);

function fail(text, code = 1) {
  process.stderr.write(text.endsWith('\n') ? text : `${text}\n`);
  process.exit(code);
}

function int(name, value, lo, hi) {
  const n = Number(value);
  if (!Number.isInteger(n) || n < lo || n > hi) fail(`--${name} braucht eine ganze Zahl von ${lo} bis ${hi}.`);
  return n;
}

async function call(httpPort, method, pathname, body) {
  let res;
  try {
    res = await fetch(`http://127.0.0.1:${httpPort}${pathname}`, {
      method,
      headers: body !== undefined ? { 'Content-Type': 'application/json' } : {},
      body: body !== undefined ? body : undefined,
    });
  } catch {
    fail(`Die Konsole läuft nicht (http://127.0.0.1:${httpPort}/ nicht erreichbar). Erst "game-console" starten.`, 2);
  }
  return res.json();
}

function printResult(result) {
  if (result.ok) {
    process.stdout.write(`${result.message ?? 'OK'}\n`);
    return;
  }
  fail(result.error ?? 'Fehler.');
}

function argsFor(action, pos) {
  switch (action) {
    case 'announce': return pos[0] ? { id: pos[0] } : {};
    case 'winner': return { n: Number(pos[0]) };
    case 'admit': return { mac: pos[0] ?? 'all', n: pos[1] !== undefined ? Number(pos[1]) : undefined };
    case 'renumber': return { n: Number(pos[0]), to: Number(pos[1]) };
    case 'release': return { n: Number(pos[0]) };
    case 'admit-mode': return { mode: pos[0] };
    case 'usb-number': return pos[0] === undefined ? {} : { n: pos[0] === 'none' ? null : Number(pos[0]) };
    default: return {};
  }
}

async function runConsole(v) {
  for (const o of ['sim-loss', 'sim-latency', 'sim-reboots']) {
    if (v.simulate === undefined && v[o] !== undefined) fail(`--${o} gibt es nur mit --simulate.`);
  }
  const simulate = v.simulate !== undefined ? int('simulate', v.simulate, 1, 999) : 0;
  if (simulate && v.port) fail('--port und --simulate schließen sich aus.');
  const stamp = () => new Date().toLocaleTimeString('de-DE');
  const tags = { info: '', warn: 'Achtung: ', error: 'Fehler: ', link: 'USB: ', tx: '> ', rx: '< ' };
  const app = await startConsole({
    port: v.port ?? null,
    simulate,
    simLoss: v['sim-loss'] !== undefined ? int('sim-loss', v['sim-loss'], 0, 100) / 100 : 0,
    simLatency: v['sim-latency'] !== undefined ? int('sim-latency', v['sim-latency'], 0, 2000) : 0,
    simReboots: !!v['sim-reboots'],
    channel: v.channel !== undefined ? int('channel', v.channel, 1, 13) : 6,
    fleet: v.fleet !== undefined ? int('fleet', v.fleet, 0, 65535) : 0,
    autoAdmit: !v['manual-admit'],
    httpPort: v['http-port'] !== undefined ? int('http-port', v['http-port'], 1, 65535) : DEFAULT_HTTP_PORT,
    inbox: v.inbox ?? DEFAULT_INBOX,
    trace: !!v.trace,
    log: (level, text) => process.stdout.write(`${stamp()} ${tags[level] ?? ''}${text}\n`),
  }).catch((err) => fail(err.code === 'EADDRINUSE' ? `Port ${v['http-port'] ?? DEFAULT_HTTP_PORT} ist belegt. Läuft die Konsole schon?` : err.message));
  process.stdout.write(`Bildschirm: ${app.url}\nEingang:    ${app.inbox.dir}\n`);
  let closing = false;
  const shutdown = async () => {
    if (closing) process.exit(0);
    closing = true;
    await app.close();
    process.exit(0);
  };
  process.on('SIGINT', shutdown);
  process.on('SIGTERM', shutdown);
}

async function main() {
  let parsed;
  try {
    parsed = parseArgs({
      allowPositionals: true,
      options: {
        port: { type: 'string' },
        simulate: { type: 'string' },
        'sim-loss': { type: 'string' },
        'sim-latency': { type: 'string' },
        'sim-reboots': { type: 'boolean' },
        channel: { type: 'string' },
        fleet: { type: 'string' },
        'manual-admit': { type: 'boolean' },
        'http-port': { type: 'string' },
        inbox: { type: 'string' },
        trace: { type: 'boolean' },
        file: { type: 'string' },
        help: { type: 'boolean', short: 'h' },
      },
    });
  } catch (err) {
    fail(`${err.message}\n\n${HELP}`);
  }
  const { values: v, positionals: pos } = parsed;
  if (v.help) {
    process.stdout.write(HELP);
    return;
  }
  const [command, ...rest] = pos;
  const httpPort = v['http-port'] !== undefined ? int('http-port', v['http-port'], 1, 65535) : DEFAULT_HTTP_PORT;

  if (!command || command === 'start') return runConsole(v);

  if (command === 'send' || command === 'round') {
    const file = v.file ?? rest[0];
    if (!file) fail(`Datei fehlt: game-console ${command === 'send' ? 'send <datei.json>' : 'round --file <datei.json>'}`);
    let text;
    try {
      text = await fs.readFile(file, 'utf8');
    } catch (err) {
      fail(`Datei ${file} nicht lesbar: ${err.message}`);
    }
    return printResult(await call(httpPort, 'POST', '/api/load', text));
  }

  if (command === 'status') {
    const s = await call(httpPort, 'GET', '/api/state');
    const link = s.link.state === 'connected' ? `verbunden (${s.link.port})` : `nicht verbunden${s.link.status ? ` - ${s.link.status}` : ''}`;
    const lines = [
      `Gateway:   ${link}`,
      `Sitzung:   ${s.session ? `${s.session.id}, Kanal ${s.session.channel}, Anmeldung ${s.session.login ? 'offen' : 'zu'}${s.session.ending ? ', endet' : ''}` : 'keine'}`,
      `Geräte:    ${s.counts.admitted} aufgenommen, ${s.counts.online} online, ${s.counts.waiting} warten`,
      `Runde:     ${s.round ? `${s.round.title || s.round.id || s.round.type} (${s.round.type}, ${s.round.phase ?? '-'})` : 'keine'}`,
      `Plan:      ${s.plan.count} Runde(n), als nächstes: ${s.plan.next ? s.plan.next.title || s.plan.next.id : '-'}`,
    ];
    process.stdout.write(`${lines.join('\n')}\n`);
    return undefined;
  }

  if (command === 'ports') {
    const { listAllPorts, ESPRESSIF_VID } = await import('../src/serial.js');
    let ports;
    try {
      ports = await listAllPorts();
    } catch (err) {
      fail(err.message);
    }
    if (!ports.length) process.stdout.write('Keine seriellen Ports gefunden.\n');
    for (const p of ports) {
      const esp = (p.vendorId || '').toLowerCase() === ESPRESSIF_VID ? '  <- Espressif' : '';
      process.stdout.write(`${p.path}  ${p.vendorId ?? '----'}:${p.productId ?? '----'}  ${p.manufacturer ?? ''}${esp}\n`);
    }
    return undefined;
  }

  if (ACTIONS.has(command)) {
    return printResult(await call(httpPort, 'POST', '/api/action', JSON.stringify({ action: command, ...argsFor(command, rest) })));
  }
  fail(`Unbekannter Befehl "${command}".\n\n${HELP}`);
  return undefined;
}

main();
