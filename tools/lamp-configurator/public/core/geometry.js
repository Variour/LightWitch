// Geometry of lamps built from 32 mm equilateral LED triangles.
// No DOM, no three.js: runs in the browser and in Node (tests).
// Units: mm. World frame: z up, origin at the centre of the lamp's foot.
// A triangle is { id, v: [[x,y,z],[x,y,z],[x,y,z]], type, out }. The vertex order defines
// the LED side: the right-hand normal (v1-v0)x(v2-v0) points out of the LED side.
// Edge k runs from v[k] to v[(k+1)%3].

export const GRID = 32;                       // grid edge (joint centre lines)
export const TRI_H = GRID * Math.sqrt(3) / 2; // 27.713
export const JOIN_TOL = 0.5;                  // vertices closer than this are the same point
export const SNAP_MM = 4;                     // magnetic snap radius for docking

export const add = (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
export const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
export const mul = (a, s) => [a[0] * s, a[1] * s, a[2] * s];
export const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
export const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
export const len = a => Math.hypot(a[0], a[1], a[2]);
export const norm = a => { const l = len(a) || 1; return [a[0] / l, a[1] / l, a[2] / l]; };
export const dist = (a, b) => len(sub(a, b));
export const mid = (a, b) => mul(add(a, b), 0.5);
const round3 = x => Math.round(x * 1000) / 1000;
export const roundV = p => p.map(round3);

export const normal = t => norm(cross(sub(t.v[1], t.v[0]), sub(t.v[2], t.v[0])));
export const centroid = t => mul(add(add(t.v[0], t.v[1]), t.v[2]), 1 / 3);
export const edgePts = (t, k) => [t.v[k], t.v[(k + 1) % 3]];

// Reorder vertices so that the LED side faces `want` (any vector with a positive dot product).
export function orient(v, want) {
  const n = cross(sub(v[1], v[0]), sub(v[2], v[0]));
  return dot(n, want) < 0 ? [v[0], v[2], v[1]] : v;
}

// Local frame of a triangle for a given board rotation r (0..2): the board's IN edge
// (datasheet edge 0, the base) lies on triangle edge r. Datasheet coordinates: base from
// (-16,0) to (16,0), apex at (0,27.713), seen from the LED side.
export function boardFrame(t, r) {
  const P = t.v[r], Q = t.v[(r + 1) % 3], R = t.v[(r + 2) % 3], M = mid(P, Q);
  const ex = norm(sub(Q, P)), ey = norm(sub(R, M));
  return { o: M, ex, ey, n: normal(t) };
}
export const toWorld = (f, x, y) => add(f.o, add(mul(f.ex, x), mul(f.ey, y)));

// Shared edges (seams) and free edges of a set of triangles.
export function topology(tris) {
  const seams = [], used = new Set();
  for (let i = 0; i < tris.length; i++) for (let k = 0; k < 3; k++) {
    const [a, b] = edgePts(tris[i], k);
    for (let j = i + 1; j < tris.length; j++) for (let m = 0; m < 3; m++) {
      const [c, d] = edgePts(tris[j], m);
      const same = (dist(a, c) < JOIN_TOL && dist(b, d) < JOIN_TOL) || (dist(a, d) < JOIN_TOL && dist(b, c) < JOIN_TOL);
      if (same) { seams.push({ a: i, ka: k, b: j, kb: m }); used.add(i + ':' + k); used.add(j + ':' + m); }
    }
  }
  const free = [];
  tris.forEach((_, i) => [0, 1, 2].forEach(k => { if (!used.has(i + ':' + k)) free.push({ i, k }); }));
  const nb = tris.map(() => []);
  seams.forEach((s, si) => { nb[s.a].push({ j: s.b, k: s.ka, seam: si }); nb[s.b].push({ j: s.a, k: s.kb, seam: si }); });
  return { seams, free, nb };
}

// Hinge geometry for docking a new triangle on free edge k of triangle t.
function hinge(t, k) {
  const P = t.v[k], Q = t.v[(k + 1) % 3], R = t.v[(k + 2) % 3], M = mid(P, Q), n = normal(t);
  const e = norm(sub(Q, P));
  let w = sub(M, R); w = norm(sub(w, mul(e, dot(w, e))));
  return { P, Q, M, n, w, e };
}
// Apex of a docked triangle at fold angle a (deg). 0 = flat continuation, positive folds
// backwards (away from the LED side), so a closed convex body with LEDs outside uses positive angles.
export function dockApex(t, k, aDeg) {
  const h = hinge(t, k), a = aDeg * Math.PI / 180;
  return add(h.M, add(mul(h.w, TRI_H * Math.cos(a)), mul(h.n, -TRI_H * Math.sin(a))));
}
// The new triangle keeps the LED side consistent with its neighbour (shared edge reversed).
export function dock(t, k, aDeg, extra = {}) {
  const P = t.v[k], Q = t.v[(k + 1) % 3];
  return { v: [Q, P, dockApex(t, k, aDeg)].map(roundV), type: t.type, out: t.out, ...extra };
}
// Fold angle (deg) of a seam: angle between the two triangles, 0 = flat, positive = folded backwards.
export function foldAngle(t, k, other) {
  const h = hinge(t, k), apex = other.v.find(p => dist(p, h.P) > JOIN_TOL && dist(p, h.Q) > JOIN_TOL);
  const d = sub(apex, h.M);
  return Math.atan2(-dot(d, h.n), dot(d, h.w)) * 180 / Math.PI;
}

// Magnetic snap: fold angles at which the new apex lands on an existing vertex (within SNAP_MM).
// Returns [{ angle, d, vertex }] sorted by distance, angle in (-180, 180].
export function snapAngles(tris, i, k, snapMm = SNAP_MM) {
  const t = tris[i], h = hinge(t, k), out = [], seen = [];
  for (const o of tris) for (const X of o.v) {
    if (dist(X, h.P) < JOIN_TOL || dist(X, h.Q) < JOIN_TOL) continue;
    if (seen.some(s => dist(s, X) < JOIN_TOL)) continue; seen.push(X);
    const d = sub(X, h.M), along = dot(d, h.e), px = dot(d, h.w), py = -dot(d, h.n);
    const a = Math.atan2(py, px) * 180 / Math.PI, apex = dockApex(t, k, a), gap = dist(apex, X);
    if (gap < snapMm && Math.abs(along) < snapMm) out.push({ angle: Math.round(a * 1000) / 1000, d: gap, vertex: X });
  }
  return out.sort((p, q) => p.d - q.d);
}
// Dock with snapping: if a snap angle is within tolDeg of the requested angle, use the exact
// snapped apex (closes the gap). Returns { tri, snapped }.
export function dockSnapped(tris, i, k, aDeg, tolDeg = 12) {
  const c = snapAngles(tris, i, k).filter(s => Math.abs(angDiff(s.angle, aDeg)) <= tolDeg)
    .sort((p, q) => Math.abs(angDiff(p.angle, aDeg)) - Math.abs(angDiff(q.angle, aDeg)))[0];
  if (!c) return { tri: dock(tris[i], k, aDeg), snapped: null };
  const tri = dock(tris[i], k, c.angle); tri.v[2] = roundV(c.vertex);
  return { tri, snapped: c };
}
const angDiff = (a, b) => ((a - b + 540) % 360) - 180;

// Would triangle `t` collide with existing ones (same place, or overlapping centroid)?
export function overlaps(tris, t) {
  const c = centroid(t);
  return tris.some(o => dist(centroid(o), c) < 8);
}

// ---------- generators ----------
const ring = (n, R, z, phase) => Array.from({ length: n }, (_, i) => {
  const a = phase + i * 2 * Math.PI / n; return [R * Math.cos(a), R * Math.sin(a), z];
});
export const ringRadius = n => GRID / (2 * Math.sin(Math.PI / n));
export const bandHeight = n => Math.sqrt(GRID * GRID - (2 * ringRadius(n) * Math.sin(Math.PI / (2 * n))) ** 2);

// Zigzag bands (uniform antiprism bands) around an n-gon, `rows` bands stacked into a straight
// cylinder. LED side outwards. Returns { tris, top, bottom, rings } (top/bottom ring vertices).
export function bands(n, rows, { z0 = 0, types = ['weiss'] } = {}) {
  const R = ringRadius(n), h = bandHeight(n), tris = [], rings = [];
  for (let j = 0; j <= rows; j++) rings.push(ring(n, R, z0 + j * h, j * Math.PI / n));
  for (let j = 0; j < rows; j++) {
    const B = rings[j], T = rings[j + 1], type = types[Math.min(j, types.length - 1)];
    for (let i = 0; i < n; i++) {
      const b0 = B[i], b1 = B[(i + 1) % n], t0 = T[i], t1 = T[(i + 1) % n];
      for (const v of [[b0, b1, t0], [b1, t1, t0]]) {
        const c = mul(add(add(v[0], v[1]), v[2]), 1 / 3);
        tris.push({ v: orient(v, [c[0], c[1], 0]).map(roundV), type, band: j });
      }
    }
  }
  return { tris, rings, bottom: rings[0], top: rings[rows] };
}
// Cap on a ring of n vertices: n triangles meeting in an apex on the axis (n = 3, 4, 5),
// flat for n = 6. dir = +1 caps the top (LEDs up/out), -1 the bottom (LEDs down/out).
export function cap(ringV, dir = 1, type = 'weiss', extra = {}) {
  const n = ringV.length, R = Math.hypot(ringV[0][0], ringV[0][1]), z = ringV[0][2];
  const hz = Math.sqrt(Math.max(0, GRID * GRID - R * R)), apex = [0, 0, z + dir * hz];
  return ringV.map((p, i) => {
    const v = [p, ringV[(i + 1) % n], apex], c = mul(add(add(v[0], v[1]), v[2]), 1 / 3);
    return { v: orient(v, [c[0], c[1], dir * Math.max(hz, 4)]).map(roundV), type, ...extra };
  });
}
// Flat area: `rows` rows of `cols` triangles, alternating up/down, in the x-z plane, LEDs to -y.
export function flat(cols, rows, { type = 'rgbmatrix1', z0 = 0 } = {}) {
  const tris = [], w = (cols + 1) / 2 * GRID;
  for (let r = 0; r < rows; r++) for (let c = 0; c < cols; c++) {
    const x0 = -w / 2 + c * GRID / 2, z = z0 + r * TRI_H, up = (c + r) % 2 === 0;
    const v = up ? [[x0, 0, z], [x0 + GRID, 0, z], [x0 + GRID / 2, 0, z + TRI_H]]
      : [[x0, 0, z + TRI_H], [x0 + GRID / 2, 0, z], [x0 + GRID, 0, z + TRI_H]];
    tris.push({ v: orient(v, [0, -1, 0]).map(roundV), type, band: r });
  }
  return tris;
}
// Platonic bodies: one 32 mm triangle centred on each face of a body with edge `a`
// (a = 32: the triangles touch and form the body; a > 32: loose triangles on a core).
export function body(kind, a = GRID, { type = 'rgb', zc = null } = {}) {
  let V = [];
  const p = (1 + Math.sqrt(5)) / 2;
  if (kind === 'ico') { for (const s1 of [-1, 1]) for (const s2 of [-1, 1]) V.push([0, s1, s2 * p], [s1, s2 * p, 0], [s2 * p, 0, s1]); V = V.map(v => mul(v, a / 2)); }
  else if (kind === 'okt') { const h = a / Math.SQRT2; V = [[h, 0, 0], [-h, 0, 0], [0, h, 0], [0, -h, 0], [0, 0, h], [0, 0, -h]]; }
  else if (kind === 'tetra') { const s = a / (2 * Math.SQRT2); V = [[s, s, s], [s, -s, -s], [-s, s, -s], [-s, -s, s]]; }
  else throw new Error('unknown body ' + kind);
  const F = [];
  for (let i = 0; i < V.length; i++) for (let j = i + 1; j < V.length; j++) for (let k = j + 1; k < V.length; k++)
    if ([dist(V[i], V[j]), dist(V[j], V[k]), dist(V[i], V[k])].every(x => Math.abs(x - a) < 0.01)) F.push([V[i], V[j], V[k]]);
  const zmin = Math.min(...V.map(v => v[2])), lift = zc == null ? -zmin : zc;
  return F.map(f => {
    const c = mul(add(add(f[0], f[1]), f[2]), 1 / 3), sc = GRID / a;
    const T = f.map(v => add(add(c, mul(sub(v, c), sc)), [0, 0, lift]));
    return { v: orient(T, c).map(roundV), type };
  });
}

// Bounding info: height, largest diameter around the z axis, smallest inner radius.
export function measure(tris) {
  if (!tris.length) return { h: 0, d: 0, w: 0, t: 0, zmin: 0, zmax: 0 };
  const P = tris.flatMap(t => t.v), zs = P.map(p => p[2]), span = k => Math.max(...P.map(p => p[k])) - Math.min(...P.map(p => p[k]));
  return { w: span(0), t: span(1), zmin: Math.min(...zs), zmax: Math.max(...zs), h: Math.max(...zs) - Math.min(...zs), d: 2 * Math.max(...P.map(p => Math.hypot(p[0], p[1]))) };
}

let idSeq = 0;
export const newId = tris => { let m = 0; for (const t of tris) { const n = +String(t.id || '').slice(1); if (n > m) m = n; } return 't' + Math.max(m + 1, ++idSeq); };
export function withIds(tris) { let n = 0; return tris.map(t => ({ ...t, id: t.id || 't' + (++n) })); }
