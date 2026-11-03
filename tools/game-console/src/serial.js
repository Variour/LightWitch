import { EventEmitter } from 'node:events';
import fs from 'node:fs';
import path from 'node:path';

export const ESPRESSIF_VID = '303a';
const RETRY_MS = 1000;

async function loadSerialPort() {
  // Loaded on demand, so the simulator runs without the native binding.
  const mod = await import('serialport');
  return mod.SerialPort ?? mod.default.SerialPort;
}

// On Linux the port list comes from udevadm (part of systemd/udev).
function checkLister() {
  if (process.platform !== 'linux') return;
  const dirs = (process.env.PATH || '').split(path.delimiter);
  if (!dirs.some((d) => d && fs.existsSync(path.join(d, 'udevadm')))) {
    throw new Error('udevadm fehlt, die Ports lassen sich nicht auflisten. Den Port mit --port angeben.');
  }
}

async function listPorts() {
  checkLister();
  const SerialPort = await loadSerialPort();
  return SerialPort.list();
}

// Every serial port with the Espressif USB vendor id (USB-Serial-JTAG of the
// ESP32-C5 and its siblings).
export async function listGatewayPorts() {
  const ports = await listPorts();
  return ports.filter((p) => (p.vendorId || '').toLowerCase() === ESPRESSIF_VID);
}

export async function listAllPorts() {
  return listPorts();
}

// USB serial transport for GatewayLink. Opens the given path, or the only
// Espressif port, and reopens it whenever it comes back (gateway unplugged
// or restarted: USB-Serial-JTAG enumerates anew).
//
// The port is opened with the serialport defaults and DTR/RTS are never set
// afterwards. POSIX asserts DTR and RTS together on open; Windows opens with
// DTR on and RTS off. An ESP32 behind USB-Serial-JTAG resets only while RTS
// is on and DTR is off, which neither default reaches.
export class SerialTransport extends EventEmitter {
  constructor({ path = null } = {}) {
    super();
    this.fixedPath = path;
    this.port = null;
    this.stopped = false;
    this.timer = null;
  }

  start() {
    this.stopped = false;
    this._attempt();
  }

  async stop() {
    this.stopped = true;
    clearTimeout(this.timer);
    const port = this.port;
    this.port = null;
    if (port && port.isOpen) {
      await new Promise((resolve) => port.close(() => resolve()));
    }
  }

  write(text) {
    if (this.port && this.port.isOpen) this.port.write(text);
  }

  _status(text) {
    this.emit('status', text);
  }

  _retry() {
    if (this.stopped) return;
    clearTimeout(this.timer);
    this.timer = setTimeout(() => this._attempt(), RETRY_MS);
  }

  async _attempt() {
    if (this.stopped || this.port) return;
    let portPath = this.fixedPath;
    try {
      const SerialPort = await loadSerialPort();
      if (!portPath) {
        const esp = await listGatewayPorts();
        if (esp.length === 0) {
          this._status('Kein Gateway am USB gefunden (Espressif 303a). Warte ...');
          this._retry();
          return;
        }
        if (esp.length > 1) {
          this._status(`Mehrere Espressif-Ports: ${esp.map((p) => p.path).join(', ')}. Mit --port einen wählen.`);
          this._retry();
          return;
        }
        portPath = esp[0].path;
      }
      this._status(`Öffne ${portPath} ...`);
      const port = new SerialPort({ path: portPath, baudRate: 115200, autoOpen: false });
      await new Promise((resolve, reject) => port.open((err) => (err ? reject(err) : resolve())));
      if (this.stopped) {
        port.close(() => {});
        return;
      }
      this.port = port;
      port.on('data', (chunk) => this.emit('data', chunk));
      port.on('close', () => {
        if (this.port !== port) return;
        this.port = null;
        this._status(`${portPath} getrennt. Warte auf das Gateway ...`);
        this.emit('close');
        this._retry();
      });
      port.on('error', () => {
        // 'close' follows when the device is gone.
      });
      this._status(`Verbunden mit ${portPath}`);
      this.emit('open', { path: portPath, sim: false });
    } catch (err) {
      this._status(`Port ${portPath ?? ''} nicht offen: ${err.message}. Neuer Versuch ...`);
      this._retry();
    }
  }
}
