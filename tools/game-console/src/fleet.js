import { normalizeMac, FW_RE, FLEET_STATES, CHARGE_STATES, CHARGE_TARGETS, UPDATE_STATES } from './protocol.js';

// A device counts as silent after three missed status periods (~30 s each).
export const FLEET_STALE_MS = 90000;
// `age` comes in whole seconds: a status up to this much older than the
// stored one still counts as the same news.
const AGE_SLACK_MS = 1500;

// `err` bits as the firmware defines them (src/fleet/FleetTypes.h, kErr*).
export const ERROR_BITS = [
  'Lader antwortet nicht',
  'Akku zu kalt',
  'Akku zu heiß',
  'Lader überhitzt',
  'Akkufehler',
  'Ladezeit überschritten',
  'Versorgungsfehler',
  'kein Temperaturfühler',
  'Akku schwach',
];

export function errorTexts(err) {
  const out = [];
  for (let bit = 0; bit < 16; bit++) {
    if (err & (1 << bit)) out.push(ERROR_BITS[bit] ?? `Fehlerbit ${bit}`);
  }
  return out;
}

const oneOf = (list, v) => (list.includes(v) ? v : null);
const int = (v) => (Number.isInteger(v) ? v : null);

// One `fleet status` line, checked and normalised. Unknown enum values become
// null so the screen shows "?" instead of guessing. Null without a valid MAC.
export function parseFleetStatus(ev) {
  const mac = normalizeMac(ev.mac);
  if (!mac) return null;
  const n = Number.isInteger(ev.n) && ev.n >= 1 && ev.n <= 999 ? ev.n : null; // 0 or null: no number
  const fw = typeof ev.fw === 'string' && FW_RE.test(ev.fw.toLowerCase()) ? ev.fw.toLowerCase() : null;
  const bat = int(ev.bat);
  return {
    mac,
    n,
    state: oneOf(FLEET_STATES, ev.state),
    bat: bat === null ? null : Math.max(0, Math.min(100, bat)),
    vbat: int(ev.vbat),
    chg: oneOf(CHARGE_STATES, ev.chg),
    target: oneOf(CHARGE_TARGETS, ev.target),
    usb: typeof ev.usb === 'boolean' ? ev.usb : null,
    fw,
    upd: oneOf(UPDATE_STATES, ev.upd),
    pct: int(ev.pct),
    err: int(ev.err) ?? 0,
    rssi: int(ev.rssi),
    ageMs: Number.isFinite(ev.age) && ev.age >= 0 ? ev.age * 1000 : 0,
  };
}

// The console's copy of the bridge's fleet table: per MAC the latest status
// the bridge heard, with the moment it heard it (`age` back from arrival).
export class FleetTable {
  constructor({ now = () => Date.now() } = {}) {
    this.now = now;
    this.devices = new Map(); // mac -> entry
    this.removed = new Map(); // mac -> time it was taken out of the table
  }

  // Returns true when the line changed the table.
  onStatus(ev) {
    const s = parseFleetStatus(ev);
    if (!s) return false;
    const now = this.now();
    const heardAt = now - s.ageMs;
    const cut = this.removed.get(s.mac);
    if (cut !== undefined) {
      if (heardAt <= cut + AGE_SLACK_MS) return false; // replay of what the device said before
      this.removed.delete(s.mac);
    }
    const old = this.devices.get(s.mac);
    if (old && heardAt < old.heardAt - AGE_SLACK_MS) return false; // older than what we have
    const fields = { ...s };
    delete fields.ageMs;
    this.devices.set(s.mac, { ...fields, heardAt, rxAt: now });
    return true;
  }

  // Takes the devices running on battery out of the table (after `off`: they
  // go to ship mode). Devices on USB ignore `off` and stay. Returns the count.
  removeOnBattery() {
    const now = this.now();
    let count = 0;
    for (const [mac, d] of this.devices) {
      if (d.usb === false) {
        this.devices.delete(mac);
        this.removed.set(mac, now);
        count++;
      }
    }
    return count;
  }

  // Devices with this number (0 or more).
  byNumber(n) {
    return [...this.devices.values()].filter((d) => d.n === n);
  }

  snapshot(refFw) {
    const now = this.now();
    const list = [...this.devices.values()]
      .sort((a, b) => (a.n ?? 1000) - (b.n ?? 1000) || a.mac.localeCompare(b.mac))
      .map((d) => {
        const stale = now - d.heardAt >= FLEET_STALE_MS;
        let fwStatus = 'unknown';
        if (d.fw && refFw) fwStatus = d.fw === refFw ? 'current' : 'outdated';
        const updating = ['checking', 'downloading', 'installing'].includes(d.upd);
        const error = d.err !== 0 || d.chg === 'fault' || d.upd === 'failed';
        return {
          mac: d.mac, n: d.n, state: d.state, bat: d.bat, vbat: d.vbat, chg: d.chg, target: d.target,
          usb: d.usb, fw: d.fw, fwStatus, upd: d.upd, pct: d.pct, updating,
          err: d.err, errors: errorTexts(d.err), error, rssi: d.rssi, lastSeen: d.heardAt, ageS: Math.max(0, Math.round((now - d.heardAt) / 1000)), stale,
        };
      });
    const fresh = list.filter((d) => !d.stale);
    return {
      devices: list,
      counts: {
        total: list.length,
        charging: fresh.filter((d) => d.chg === 'charging').length,
        full: fresh.filter((d) => d.chg === 'full').length,
        outdated: fresh.filter((d) => d.fwStatus === 'outdated').length,
        updating: fresh.filter((d) => d.updating).length,
        errors: fresh.filter((d) => d.error).length,
        silent: list.length - fresh.length,
      },
    };
  }
}
