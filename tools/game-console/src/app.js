import os from 'node:os';
import path from 'node:path';
import { GatewayLink } from './link.js';
import { GameConsole } from './game.js';
import { startServer } from './server.js';
import { Inbox } from './inbox.js';
import { SerialTransport } from './serial.js';
import { FakeGateway } from './sim/fake-gateway.js';

export const DEFAULT_HTTP_PORT = 4711;
export const DEFAULT_INBOX = path.join(os.homedir(), 'buzzer-inbox');

// Starts the whole console: transport (USB or simulator), link, console
// state, local HTTP server with the screen, and the inbox watcher.
export async function startConsole({
  port = null,
  simulate = 0,
  simLoss = 0,
  simLatency = 0,
  simReboots = false,
  simSeed = 1,
  simTiming = {},
  channel = 6,
  fleet = 0,
  autoAdmit = true,
  httpPort = DEFAULT_HTTP_PORT,
  inbox = DEFAULT_INBOX,
  log = () => {},
  trace = false,
} = {}) {
  const transport = simulate > 0
    ? new FakeGateway({ devices: simulate, loss: simLoss, latencyMs: simLatency, reboots: simReboots, seed: simSeed, timing: simTiming })
    : new SerialTransport({ path: port });
  const link = new GatewayLink(transport);
  const game = new GameConsole({ link, channel, fleet, autoAdmit, mode: simulate > 0 ? 'sim' : 'serial' });

  game.on('message', ({ level, text }) => log(level, text));
  game.on('rejected', ({ source, result }) => log('error', `${source}: ${result.error}`));
  transport.on('status', (text) => log('link', text));
  if (trace) {
    link.on('tx', (obj) => log('tx', JSON.stringify(obj)));
    link.on('rx', (ev) => log('rx', JSON.stringify(ev)));
  }

  const server = await startServer({ game, port: httpPort });
  const box = inbox ? new Inbox({ dir: inbox, apply: (input) => game.load(input) }) : null;
  if (box) {
    box.on('processed', ({ name, result }) => {
      if (!result.ok) {
        game.note('error', `Datei ${name} abgelehnt. ${result.error.split('\n').slice(1).join(' ')}`);
      }
    });
    box.on('problem', (text) => game.note('error', text));
    await box.start();
  }
  link.start();

  return {
    game,
    link,
    transport,
    server,
    inbox: box,
    url: server.url,
    async close() {
      if (box) box.stop();
      await link.stop();
      await server.close();
    },
  };
}
