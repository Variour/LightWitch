import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { topology, dock, snapAngles, dockSnapped, normal, centroid, dot, sub, dist, foldAngle, GRID, measure } from '../public/core/geometry.js';
import { makePreset, CONTROLLERS } from '../public/core/catalog.js';
import { planWiring } from '../public/core/wiring.js';
import { analyse, partsList, withStock, consume, toCsv } from '../public/core/bom.js';
import { lampFile, projectFile, readProject, pack, unpack, deviceUrl, placement } from '../public/core/export.js';

const modules = JSON.parse(readFileSync(new URL('../public/data/lamp-modules.json', import.meta.url)));
const edgeLen = t => [0, 1, 2].map(k => dist(t.v[k], t.v[(k + 1) % 3]));

const expected = { teelicht: 3, oellampe: 28, allrounder: 72, kugel: 20, wurfball: 8, matrix: 252, leer: 1 };
for (const [key, n] of Object.entries(expected)) {
  test(`preset ${key}: ${n} triangles, 32 mm edges, LEDs outwards, complete chains`, () => {
    const p = makePreset(key);
    assert.equal(p.tris.length, n);
    p.tris.forEach(t => edgeLen(t).forEach(l => assert.ok(Math.abs(l - GRID) < 0.05, `edge ${l}`)));
    const topo = topology(p.tris);
    const w = planWiring(p.tris, CONTROLLERS[p.controller].outputs, topo);
    const covered = w.chains.flatMap(c => c.path).sort((a, b) => a - b);
    assert.deepEqual(covered, p.tris.map((_, i) => i), 'every triangle in exactly one chain');
    assert.ok(!w.warnings.some(x => x.code === 'unassigned'));
    // closed and stacked shapes need no jumpers; loose balls are all jumpers
    const jumps = w.chains.reduce((s, c) => s + c.jumps.length, 0);
    if (key === 'kugel' || key === 'wurfball') assert.equal(jumps, n - 1);
    else assert.equal(jumps, 0, 'no jumpers');
    // chain step goes over the predecessor's seam and the IN edge sits on it
    for (const c of w.chains) c.path.forEach((ti, k) => {
      if (!k || w.per[ti].inFrom !== 'seam') return;
      const s = topo.nb[ti].find(x => x.j === c.path[k - 1]);
      assert.equal(w.per[ti].rot, s.k);
    });
    // LED sides point away from the body centre (not for the flat matrix)
    if (key !== 'matrix' && key !== 'leer') {
      const P = p.tris.flatMap(t => t.v), c0 = P.reduce((s, v) => s.map((x, i) => x + v[i] / P.length), [0, 0, 0]);
      const outward = p.tris.filter(t => dot(normal(t), sub(centroid(t), c0)) > 0).length;
      const down = key === 'allrounder' ? 18 : 0; // the star collar faces down on purpose
      assert.ok(outward >= n - down, `${outward}/${n} outward`);
    }
  });
}

test('closed shapes have no free edges except their openings', () => {
  const t = topology(makePreset('teelicht').tris);
  assert.equal(t.seams.length, 3); assert.equal(t.free.length, 3);
  const o = topology(makePreset('oellampe').tris);
  assert.equal(o.free.length, 4, 'only the open bottom square');
  const a = topology(makePreset('allrounder').tris);
  assert.equal(a.free.length, 6 + 6, 'collar rim: 6 outer edges + 6 gap triangles');
});

test('allrounder groups follow the catalog', () => {
  const p = makePreset('allrounder'), n = {};
  p.tris.forEach(t => { const k = t.out + ':' + t.type; n[k] = (n[k] || 0) + 1; });
  assert.equal(n['A:kerze'], 12); assert.equal(n['B:rgb'], 18); assert.equal(n['C:weiss'] + n['D:weiss'], 42);
  const m = measure(p.tris); assert.ok(m.d > 120 && m.d < 135, 'collar Ø ' + m.d);
});

test('dock + fold angle round trip, and magnetic snap closes a ring', () => {
  const tris = makePreset('teelicht').tris.slice(0, 2);
  const t1 = dock(tris[0], 0, 35);
  assert.ok(Math.abs(foldAngle(tris[0], 0, t1) - 35) < 0.01);
  // remove the third tealight face and snap it back
  const full = makePreset('teelicht').tris, two = full.slice(0, 2), topo = topology(two);
  const cand = topo.free.map(f => ({ f, s: snapAngles(two, f.i, f.k) })).filter(x => x.s.length);
  assert.ok(cand.length > 0, 'a snap candidate exists');
  const { f, s } = cand[0];
  const r = dockSnapped(two, f.i, f.k, s[0].angle + 5);
  assert.ok(r.snapped);
  assert.equal(topology([...two, r.tri]).seams.length, 3, 'ring closed');
});

test('bridges: chain, power and ground seams', () => {
  const p = makePreset('oellampe'), w = planWiring(p.tris, ['A', 'B']);
  const k = { kette: 0, strom: 0, masse: 0 }; w.seams.forEach(s => k[s.kind]++);
  assert.equal(k.kette, 28 - 2, 'one chain seam per step, two chains');
  assert.ok(k.masse > 0 && k.strom > 0);
  assert.equal(k.kette + k.strom + k.masse, topology(p.tris).seams.length);
});

test('locked chain is kept', () => {
  const p = makePreset('teelicht'); const w0 = planWiring(p.tris, ['A', 'B']);
  const rev = w0.chains[0].path.slice().reverse(); rev.forEach((ti, k) => { p.tris[ti].pos = k; });
  const w = planWiring(p.tris, ['A', 'B']);
  assert.deepEqual(w.chains[0].path, rev); assert.ok(w.chains[0].locked);
});

test('parts list, stock and analysis', () => {
  const p = makePreset('allrounder'), w = planWiring(p.tris, ['A', 'B', 'C', 'D']);
  const { rows, pins } = partsList(p, w, modules);
  const q = Object.fromEntries(rows.map(r => [r.key, r.qty]));
  assert.equal(q['tri-weiss'], 42); assert.equal(q['tri-kerze'], 12); assert.equal(q['tri-rgb'], 18);
  assert.equal(q['ctl-L'], 1); assert.equal(q.cell18650, 6); assert.equal(q.cable3, 4);
  assert.ok(pins > 0);
  const st = withStock(rows, { 'tri-weiss': 50, 'tri-kerze': 5 });
  assert.equal(st.find(r => r.key === 'tri-weiss').missing, 0); assert.equal(st.find(r => r.key === 'tri-kerze').missing, 7);
  assert.equal(consume(rows, { 'tri-weiss': 50 })['tri-weiss'], 8);
  assert.match(toCsv(rows, {}), /^Teil;Menge/);
  const a = analyse(p, w, modules);
  assert.equal(a.tris, 72); assert.ok(a.lm > 500 && a.contW <= 72 * 0.3 + 1e-9);
  assert.ok(a.hoursCont > 1 && a.hoursIdle > a.hoursCont);
});

test('lamp file: modules in chain order, frame orthonormal, IN edge at the base', async () => {
  const p = makePreset('oellampe'), w = planWiring(p.tris, ['A', 'B']), f = lampFile(p, w, modules);
  assert.equal(f.format, 'lightwitch-lamp'); assert.equal(f.version, 1);
  assert.deepEqual(Object.keys(f.types).sort(), ['kerze', 'rgb', 'weiss']);
  assert.equal(f.outputs.reduce((s, o) => s + o.modules.length, 0), 28);
  for (const o of f.outputs) for (const m of o.modules) {
    assert.ok(Math.abs(dot(m.n, m.u)) < 1e-3); assert.ok(Math.abs(Math.hypot(...m.u) - 1) < 1e-3);
  }
  const t = p.tris[0], pl = placement(t, 1);
  // base of the module (y = -9.238 at x = 0) is the midpoint of edge 1
  const base = pl.p.map((x, i) => x - 9.2376 * pl.u[i]), m1 = t.v[1].map((x, i) => (x + t.v[2][i]) / 2);
  assert.ok(dist(base, m1) < 0.01);
  const s = await pack(f); assert.ok(s.length < 8000, 'packed ' + s.length);
  assert.deepEqual(await unpack(s), JSON.parse(JSON.stringify(f)));
});

test('matrix lamp file stays small enough for a link', async () => {
  const p = makePreset('matrix'), w = planWiring(p.tris, ['A', 'B', 'C', 'D']);
  const s = await pack(lampFile(p, w, modules));
  assert.ok(s.length < 32000, 'packed ' + s.length);
});

test('project file round trip and device url', () => {
  const p = makePreset('kugel'), f = projectFile(p), r = readProject(JSON.parse(JSON.stringify(f)));
  assert.equal(r.tris.length, 20);
  assert.throws(() => readProject({ format: 'x' }));
  assert.equal(deviceUrl('laterne'), 'http://laterne.local/');
  assert.equal(deviceUrl('192.168.1.20'), 'http://192.168.1.20/');
  assert.equal(deviceUrl('http://lw.local/'), 'http://lw.local/');
});
