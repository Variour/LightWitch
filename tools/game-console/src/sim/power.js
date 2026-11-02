// Power, charging and fleet behaviour of a simulated Buzzerboard, following
// the fleet protocol v0 draft: charging in the transport case, active,
// standby, ship mode after `off`, the charge target, and updates with
// progress. Fleet status goes out only outside a game session.

export const FLEET_T = {
  fleetStatusPeriod: 30000, fleetStatusJitter: 3000, fleetFirstStatusMax: 3000,
  fleetUpdateStatusPeriod: 2000, // while an update runs, so the console shows progress
  fleetCheckMin: 2000, fleetCheckMax: 4000,
  fleetDownloadMin: 24000, fleetDownloadMax: 48000,
  fleetInstallMin: 3000, fleetInstallMax: 6000,
  fleetRebootMs: 2000,
  fleetTakeOutMin: 300, fleetTakeOutMax: 4000, // people take the buzzers out of the case
};

const TARGET_PCT = { store: 80, event: 100 };
const CHARGE_RATE = 0.05; // % per second up to 80 %
const CHARGE_RATE_CV = 0.02; // % per second above 80 %
const DRAIN = { active: 0.004, standby: 0.0005 };
const AP_MS = 10 * 60 * 1000;

export const prFirmware = (pr) => ((Math.imul(pr, 2654435761) >>> 0).toString(16).padStart(8, '0'));

// Rough open-circuit voltage of a Li-ion cell for a charge level.
function ocvMv(pct) {
  const pts = [[0, 3300], [10, 3600], [20, 3700], [50, 3800], [80, 4000], [100, 4180]];
  for (let i = 1; i < pts.length; i++) {
    const [p1, v1] = pts[i];
    const [p0, v0] = pts[i - 1];
    if (pct <= p1) return Math.round(v0 + ((pct - p0) / (p1 - p0)) * (v1 - v0));
  }
  return 4180;
}

export class SimPower {
  // state: charging (in the case on USB), active or standby (on battery).
  constructor(world, dev, { state = 'charging', bat = 60, fw, fault = false }) {
    this.world = world;
    this.dev = dev;
    this.state = state;
    this.usb = state === 'charging';
    this.bat = bat;
    this.target = 'store';
    this.fw = fw;
    this.fault = fault;
    this.upd = 'idle';
    this.pct = 0;
    this.update = null; // { phase, until, dlStart, dlEnd, fw }
    this.silentUntil = 0; // restarting after an update
    this.apUntil = 0;
    this.takeOutAt = 0;
    this.nonces = [];
    this.lastTick = null;
    this.dirty = false;
    this.lastChg = this.chg;
    this.nextStatusAt = world.now() + world.rnd.int(200, world.ft.fleetFirstStatusMax);
  }

  get chg() {
    if (!this.usb) return 'discharging';
    if (this.fault) return 'fault';
    return this.bat < TARGET_PCT[this.target] ? 'charging' : 'full';
  }

  get err() {
    return this.fault && this.usb ? 0x04 : 0; // battery NTC hot
  }

  get vbat() {
    const ocv = ocvMv(this.bat);
    return this.chg === 'charging' ? Math.min(ocv + 60, this.target === 'event' ? 4200 : 4050) : ocv;
  }

  get updating() {
    return this.update !== null;
  }

  // The device does nothing but charge, update or restart: no invites, no keys.
  blocksGame(now) {
    return this.state === 'charging' || this.state === 'off' || this.updating || now < this.silentUntil;
  }

  wake() {
    if (this.state === 'standby') {
      this.state = 'active';
      this.dirty = true;
    }
  }

  // Someone pulls the buzzer out of the case.
  planTakeOut(now) {
    if (!this.usb || this.takeOutAt) return;
    const t = this.world.ft;
    this.takeOutAt = now + this.world.rnd.int(t.fleetTakeOutMin, t.fleetTakeOutMax);
  }

  _unplug() {
    this.takeOutAt = 0;
    this.usb = false;
    this.fault = false;
    this.state = 'active';
    this.dirty = true;
  }

  onCommand(f, now) {
    if (this.state === 'off' || now < this.silentUntil) return;
    if (this.nonces.includes(f.nonce)) return;
    this.nonces.push(f.nonce);
    if (this.nonces.length > 16) this.nonces.shift();
    if (f.targets && !f.targets.includes(this.dev.mac)) return;
    const t = this.world.ft;
    const rnd = this.world.rnd;
    if (f.op === 'update') {
      if (this.updating) return;
      const fw = f.source === 'pr' ? prFirmware(f.pr) : this.world.releaseFw;
      this.update = { phase: 'checking', until: now + rnd.int(t.fleetCheckMin, t.fleetCheckMax), fw };
      this.upd = 'checking';
      this.pct = 0;
      this.dirty = true;
    } else if (f.op === 'charge') {
      if (this.target !== f.target) {
        this.target = f.target;
        this.dirty = true;
      }
    } else if (f.op === 'off') {
      if (!this.usb) this.state = 'off'; // ship mode; devices on USB ignore it
    } else if (f.op === 'ap') {
      this.apUntil = now + AP_MS;
    }
  }

  _tickUpdate(now) {
    const u = this.update;
    const t = this.world.ft;
    const rnd = this.world.rnd;
    if (u.phase === 'downloading') {
      const pct = Math.min(99, Math.floor(((now - u.dlStart) / (u.dlEnd - u.dlStart)) * 100));
      if (pct !== this.pct) this.pct = pct;
    }
    if (now < u.until) return;
    if (u.phase === 'checking') {
      if (this.fw === u.fw) {
        this.update = null;
        this.upd = 'idle';
      } else {
        Object.assign(u, { phase: 'downloading', dlStart: now, dlEnd: now + rnd.int(t.fleetDownloadMin, t.fleetDownloadMax) });
        u.until = u.dlEnd;
        this.upd = 'downloading';
      }
    } else if (u.phase === 'downloading') {
      u.phase = 'installing';
      u.until = now + rnd.int(t.fleetInstallMin, t.fleetInstallMax);
      this.upd = 'installing';
      this.pct = 100;
    } else {
      // Restart into the new firmware.
      this.fw = u.fw;
      this.update = null;
      this.upd = 'idle';
      this.pct = 0;
      this.silentUntil = now + t.fleetRebootMs;
      this.nextStatusAt = this.silentUntil;
      return;
    }
    this.dirty = true;
  }

  tick(now) {
    const dt = this.lastTick === null ? 0 : (now - this.lastTick) / 1000;
    this.lastTick = now;
    if (this.state === 'off') return;
    if (this.takeOutAt && now >= this.takeOutAt) this._unplug();
    if (this.usb) {
      const limit = TARGET_PCT[this.target];
      if (!this.fault && this.bat < limit) this.bat = Math.min(limit, this.bat + dt * (this.bat < 80 ? CHARGE_RATE : CHARGE_RATE_CV));
    } else {
      this.bat = Math.max(0, this.bat - dt * (DRAIN[this.state] ?? 0));
    }
    if (this.updating) this._tickUpdate(now);
    if (this.chg !== this.lastChg) {
      this.lastChg = this.chg;
      this.dirty = true;
    }
    if (this.dev.state !== 'normal' || now < this.silentUntil) return; // no fleet status in a game session
    if (this.dirty || now >= this.nextStatusAt) this._sendStatus(now);
  }

  _sendStatus(now) {
    const t = this.world.ft;
    this.dirty = false;
    this.nextStatusAt = now + (this.updating
      ? t.fleetUpdateStatusPeriod
      : this.world.rnd.int(t.fleetStatusPeriod - t.fleetStatusJitter, t.fleetStatusPeriod + t.fleetStatusJitter));
    this.world.radio.fleetFromDevice(this.dev, {
      type: 'fleetStatus', origin: this.dev.number || 0, mac: this.dev.mac, number: this.dev.number || 0,
      state: this.state, battery: Math.round(this.bat), vbat: this.vbat, charge: this.chg, target: this.target,
      usbPresent: this.usb, fw: this.fw, upd: this.upd, pct: this.pct, err: this.err,
    });
  }
}
