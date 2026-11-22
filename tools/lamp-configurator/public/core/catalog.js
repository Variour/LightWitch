// Parts catalog and example lamps. Module types (pixels, channels, lumen) live in
// data/lamp-modules.json in the LightWitch lamp format; this file adds what only the
// configurator needs (colour, price, stock item names) and the catalog lamps.
import { bands, cap, body, flat, add, sub, mul, norm, cross, dot, orient, roundV, GRID, TRI_H, withIds, centroid } from './geometry.js';

// Editor colours follow the catalog cards. Prices: USD per board at the 2027 panel plan (estimates).
export const TYPE_UI = {
  kerze:   { short: 'K',  color: '#e8a04a', price: 0.64, sub: '1 WWA + 12 Amber' },
  glimmen: { short: 'G',  color: '#c8662e', price: 0.67, sub: '15 Amber', draft: 'Vorläufig: Wie die 15 Amber-LEDs auf die ICs verteilt werden, ist noch offen. Pixel und Lumen können sich ändern.' },
  weiss:   { short: 'W',  color: '#e9eef2', price: 0.34, sub: '3 WWA' },
  rgb:     { short: 'R',  color: '#7fa8e8', price: 0.41, sub: '1 WWA + 3 RGB' },
  rgbmatrix:   { short: 'M3', color: '#b48ae0', price: 0.28, sub: '3 RGB' },
  rgbmatrix1:   { short: 'M1', color: '#8ad0c8', price: 0.14, sub: '1 RGB' },
};
export const TYPE_ORDER = ['kerze', 'glimmen', 'weiss', 'rgb', 'rgbmatrix', 'rgbmatrix1'];

// Control boards. maxMA = total LED current at 5 V the board supplies; outMA = per output switch.
export const CONTROLLERS = {
  S: { name: 'Steuerboard S', sub: 'Ø 36, 1S, ESP32-C3', outputs: ['A', 'B'], maxMA: 1200, outMA: 1200, eff: 0.87, price: 7.38, cells: 1 },
  L: { name: 'Steuerboard L', sub: '34 × 85, 2S, ESP32-C5, Solar', outputs: ['A', 'B', 'C', 'D'], maxMA: 5000, outMA: 7000, eff: 0.92, price: 17.98, cells: 2 },
};
// Battery packs. s = cells in series (0 = external 5 V supply).
export const PACKS = {
  '1s1p': { name: '1S1P LiPo 500 mAh', s: 1, wh: 1.85, items: [['lipo500', 1]] },
  '1s3p': { name: '1S3P, 3 × 18650', s: 1, wh: 32.4, items: [['cell18650', 3]] },
  '1s4p': { name: '1S4P, 4 × 18650', s: 1, wh: 43.2, items: [['cell18650', 4]] },
  '2s2p': { name: '2S2P, 4 × 18650', s: 2, wh: 43.2, items: [['cell18650', 4]] },
  '2s3p': { name: '2S3P, 6 × 18650', s: 2, wh: 64.8, items: [['cell18650', 6]] },
  psu:    { name: 'Netzteil 5 V', s: 0, wh: Infinity, items: [['psu5v', 1]] },
};
// Stock items besides the triangles and boards.
export const ITEMS = {
  lipo500:   { name: 'LiPo 500 mAh (ca. 25 × 25 × 10)', price: 3.0 },
  cell18650: { name: 'Zelle 18650', price: 3.5 },
  psu5v:     { name: 'Netzteil 5 V', price: 12 },
  bridge5:   { name: 'Brücke Kette (Stiftleiste 5 Pins)', price: 0.02 },
  bridge4:   { name: 'Brücke Strom (Stiftleiste 5 Pins, mittlerer Pin gezogen)', price: 0.02 },
  bridge2:   { name: 'Brücke Masse (2 Einzelpins auf den äußeren GND-Pads)', price: 0.01 },
  cable3:    { name: 'Kabel 3-adrig zum ersten Dreieck', price: 0.1 },
  jumper3:   { name: 'Drahtbrücke 3-adrig (Kettensprung)', price: 0.1 },
};

// Physics assumptions, same as the light studio (Lichtstudio v7).
export const PHYS = { contWPerTri: 0.3, icIdleW: 0.0025, espW: 0.03, v: 5 };

// ---------- catalog lamps ----------
// Sphere intersection: point at distance r from a, b, c, the one farther from the z axis.
function tri3(a, b, c, r) {
  const exv = sub(b, a), d = Math.hypot(...exv), ex = mul(exv, 1 / d), ac = sub(c, a), i = dot(ex, ac);
  const ey = norm(sub(ac, mul(ex, i))), ez = cross(ex, ey), j = dot(ey, ac);
  const x = d / 2, y = (i * i + j * j) / (2 * j) - i / j * x, z = Math.sqrt(Math.max(0, r * r - x * x - y * y));
  const s = [1, -1].map(k => add(add(add(a, mul(ex, x)), mul(ey, y)), mul(ez, k * z)));
  return s.sort((p, q) => Math.hypot(q[0], q[1]) - Math.hypot(p[0], p[1]))[0];
}
// Star collar on the top hexagon of a ring stack: 6 triangles folded `foldDeg` up and out,
// 12 filling the gaps, LEDs facing down.
function starCollar(top, foldDeg = 30, type = 'weiss') {
  const n = top.length, f = foldDeg * Math.PI / 180, down = [0, 0, -1], A = [], out = [];
  for (let i = 0; i < n; i++) {
    const a = top[i], b = top[(i + 1) % n], m = mul(add(a, b), 0.5), d = norm([m[0], m[1], 0]);
    A.push({ a, b, tip: add(m, add(mul(d, TRI_H * Math.cos(f)), [0, 0, TRI_H * Math.sin(f)])) });
  }
  A.forEach(o => out.push({ v: orient([o.a, o.b, o.tip], down).map(roundV), type }));
  A.forEach((o, i) => {
    const nx = A[(i + 1) % n], V = o.b, c = tri3(V, o.tip, nx.tip, GRID);
    out.push({ v: orient([V, o.tip, c], down).map(roundV), type }, { v: orient([V, c, nx.tip], down).map(roundV), type });
  });
  return out;
}
const lift = (tris, z) => { const z0 = Math.min(...tris.flatMap(t => t.v.map(p => p[2]))); return tris.map(t => ({ ...t, v: t.v.map(p => roundV([p[0], p[1], p[2] - z0 + z])) })); };
const byAngle = t => { const c = centroid(t); return Math.atan2(c[1], c[0]); };

export const PRESETS = {
  teelicht: {
    name: 'Teelicht', sub: '3 Kerze, Steuerboard S, LiPo', controller: 'S', pack: '1s1p',
    make() { const R = GRID / Math.sqrt(3), ring = [0, 1, 2].map(k => [R * Math.cos(k * 2 * Math.PI / 3 + Math.PI / 2), R * Math.sin(k * 2 * Math.PI / 3 + Math.PI / 2), 0]); return cap(ring, 1, 'kerze', { out: 'A' }); },
  },
  oellampe: {
    name: 'Öllampe', sub: '28 Dreiecke, Steuerboard S, 1S4P', controller: 'S', pack: '1s4p',
    make() {
      const b = bands(4, 3, { types: ['weiss'] }), tip = cap(b.top, 1, 'kerze');
      const all = [...b.tris, ...tip];
      // A = tip + 6 adjacent faces of the top band (Kerze), B = the rest (12 Weiß, bottom 6 RGB)
      const top = b.tris.filter(t => t.band === 2).sort((p, q) => byAngle(p) - byAngle(q));
      const A = new Set([...tip, ...top.slice(0, 6)]);
      const rest = all.filter(t => !A.has(t)).sort((p, q) => centroid(q)[2] - centroid(p)[2]);
      A.forEach(t => { t.type = 'kerze'; t.out = 'A'; });
      rest.forEach((t, i) => { t.type = i < 12 ? 'weiss' : 'rgb'; t.out = 'B'; });
      return all;
    },
  },
  allrounder: {
    name: 'Allrounder', sub: '4 × 12er-Ring, Sternkragen, Boden (72), Steuerboard L, 2S3P', controller: 'L', pack: '2s3p',
    make() {
      const b = bands(6, 4, { types: ['rgb', 'kerze', 'weiss', 'weiss'] });
      const collar = starCollar(b.top, 30, 'weiss'), floor = cap(b.bottom, -1, 'rgb');
      const all = [...b.tris, ...collar, ...floor];
      all.forEach(t => {
        if (t.type === 'kerze') t.out = 'A';
        else if (t.type === 'rgb') t.out = 'B';
        // two halves of 21; the 15° offset keeps both halves passable as one chain each
        else t.out = (byAngle(t) - Math.PI / 12 + 4 * Math.PI) % (2 * Math.PI) < Math.PI ? 'C' : 'D';
      });
      return lift(all, 0);
    },
  },
  kugel: {
    name: 'Magische Kugel', sub: '20 RGB Ambiente auf Ikosaeder-Kern, Ø 110', controller: 'S', pack: '1s1p',
    make() { return body('ico', 45, { type: 'rgb', zc: 55 }).map(t => ({ ...t, out: 'A' })); },
  },
  wurfball: {
    name: 'Wurfball', sub: '8 RGB Ambiente auf Oktaeder-Kern, Ø 90', controller: 'S', pack: '1s1p',
    make() { return body('okt', 58.8, { type: 'rgb', zc: 45 }).map(t => ({ ...t, out: 'A' })); },
  },
  matrix: {
    name: 'RGB-Matrix', sub: '252 RGB Matrix, ca. 600 × 200, Steuerboard L, Netzteil', controller: 'L', pack: 'psu',
    make() {
      const tris = flat(36, 7, { type: 'rgbmatrix' });
      // column bands 10/8/10/8: a band of 9 columns has no single chain through it
      tris.forEach((t, i) => { const c = i % 36; t.out = c < 10 ? 'A' : c < 18 ? 'B' : c < 28 ? 'C' : 'D'; });
      return tris;
    },
  },
  leer: {
    name: 'Leer', sub: 'ein Dreieck zum Anbauen', controller: 'L', pack: '2s2p',
    make() { return [{ v: [[-16, 0, 0], [16, 0, 0], [0, 0, TRI_H]].map(roundV), type: 'weiss', out: 'A' }].map(t => ({ ...t, v: orient(t.v, [0, -1, 0]) })); },
  },
};
export function makePreset(key) {
  const P = PRESETS[key];
  return { format: 'lightwitch-lamp-project', version: 1, name: P.name, controller: P.controller, pack: P.pack, tris: withIds(P.make()) };
}
