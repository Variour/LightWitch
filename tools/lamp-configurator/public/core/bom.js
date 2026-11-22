// Parts list, stock check, power and runtime.
import { TYPE_UI, TYPE_ORDER, CONTROLLERS, PACKS, ITEMS, PHYS } from './catalog.js';

// Full-load figures of one module type from the lamp-modules data.
export function typeStats(type) {
  let lm = 0, mA = 0;
  for (const px of type.pixels) for (const c of px.ch) { lm += c.lm || 0; mA += c.mA || 0; }
  return { lm, mA, pixels: type.pixels.length };
}

export function analyse(project, wiring, modules) {
  const ctl = CONTROLLERS[project.controller], pack = PACKS[project.pack];
  const counts = {}; project.tris.forEach(t => { counts[t.type] = (counts[t.type] || 0) + 1; });
  const outs = {};
  ctl.outputs.forEach(o => { outs[o] = { tris: 0, pixels: 0, mA: 0, lm: 0 }; });
  let lm = 0, mA = 0, ics = 0;
  project.tris.forEach(t => {
    const s = typeStats(modules.types[t.type]); lm += s.lm; mA += s.mA; ics += s.pixels;
    const o = outs[t.out]; if (o) { o.tris++; o.pixels += s.pixels; o.mA += s.mA; o.lm += s.lm; }
  });
  const n = project.tris.length, fullW = mA / 1000 * PHYS.v;
  const contW = Math.min(fullW, n * PHYS.contWPerTri, ctl.maxMA / 1000 * PHYS.v * (pack.s ? 1 : Infinity));
  const idleW = ics * PHYS.icIdleW;
  const hours = w => (pack.wh === Infinity ? Infinity : pack.wh / ((w + idleW) / ctl.eff + PHYS.espW));
  const warnings = [];
  if (pack.s && pack.s !== ctl.cells) warnings.push({ code: 'pack', msg: `${pack.name} passt nicht zu ${ctl.name} (${ctl.cells}S).` });
  if (!pack.s && project.controller !== 'L') warnings.push({ code: 'psu', msg: 'Netzteil-Eingang gibt es nur auf Steuerboard L.' });
  const A = mA => (mA / 1000).toLocaleString('de-DE', { maximumFractionDigits: 1 }) + ' A';
  Object.entries(outs).forEach(([id, o]) => { if (o.mA > ctl.outMA) warnings.push({ code: 'outmax', out: id, msg: `Ausgang ${id} zieht bei Vollast ${A(o.mA)}, der Schalter schafft ${A(ctl.outMA)}.` }); });
  if (pack.s && mA > ctl.maxMA) warnings.push({ code: 'limit', msg: `Vollast ${A(mA)}, ${ctl.name} liefert ${A(ctl.maxMA)}. LightWitch begrenzt den Strom.` });
  return {
    counts, outs, tris: n, pixels: ics, lm, mA, fullW, contW, idleW,
    contLm: fullW ? lm * contW / fullW : 0,
    hoursFull: hours(Math.min(fullW, ctl.maxMA / 1000 * PHYS.v)), hoursCont: hours(contW), hoursIdle: hours(0),
    warnings,
  };
}

// Parts list rows: { key, name, qty, price }
export function partsList(project, wiring, modules) {
  const rows = [], ctl = CONTROLLERS[project.controller], pack = PACKS[project.pack];
  const counts = {}; project.tris.forEach(t => { counts[t.type] = (counts[t.type] || 0) + 1; });
  TYPE_ORDER.filter(k => counts[k]).forEach(k => rows.push({ key: 'tri-' + k, name: 'LED-Dreieck ' + modules.types[k].name, qty: counts[k], price: TYPE_UI[k].price }));
  rows.push({ key: 'ctl-' + project.controller, name: ctl.name, qty: 1, price: ctl.price });
  pack.items.forEach(([k, q]) => rows.push({ key: k, name: ITEMS[k].name, qty: q, price: ITEMS[k].price }));
  const kinds = { kette: 0, strom: 0, masse: 0 }; wiring.seams.forEach(s => kinds[s.kind]++);
  if (kinds.kette) rows.push({ key: 'bridge5', name: ITEMS.bridge5.name, qty: kinds.kette, price: ITEMS.bridge5.price });
  if (kinds.strom) rows.push({ key: 'bridge4', name: ITEMS.bridge4.name, qty: kinds.strom, price: ITEMS.bridge4.price });
  if (kinds.masse) rows.push({ key: 'bridge2', name: ITEMS.bridge2.name, qty: kinds.masse, price: ITEMS.bridge2.price });
  const cables = wiring.chains.length, jumps = wiring.chains.reduce((s, c) => s + c.jumps.length, 0);
  if (cables) rows.push({ key: 'cable3', name: ITEMS.cable3.name, qty: cables, price: ITEMS.cable3.price });
  if (jumps) rows.push({ key: 'jumper3', name: ITEMS.jumper3.name, qty: jumps, price: ITEMS.jumper3.price });
  const pins = kinds.kette * 5 + kinds.strom * 4 + kinds.masse * 2;
  return { rows, pins, total: rows.reduce((s, r) => s + r.qty * r.price, 0) };
}

// Stock: { key: qty }. Returns rows with have/missing.
export function withStock(rows, stock = {}) {
  return rows.map(r => { const have = +stock[r.key] || 0; return { ...r, have, missing: Math.max(0, r.qty - have) }; });
}
export function consume(rows, stock = {}) {
  const s = { ...stock }; rows.forEach(r => { s[r.key] = Math.max(0, (+s[r.key] || 0) - r.qty); }); return s;
}
export function toCsv(rows, stock) {
  const esc = v => (/[;"\n]/.test(String(v)) ? '"' + String(v).replace(/"/g, '""') + '"' : String(v));
  const lines = [['Teil', 'Menge', 'Bestand', 'Fehlt', 'Preis USD', 'Summe USD'].join(';')];
  withStock(rows, stock).forEach(r => lines.push([r.name, r.qty, r.have, r.missing, r.price.toFixed(2), (r.qty * r.price).toFixed(2)].map(esc).join(';')));
  return lines.join('\n') + '\n';
}
