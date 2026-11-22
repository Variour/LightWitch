// Lamp configurator UI: 3D view, steps, panels. All lamp logic lives in ../core.
import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { topology, boardFrame, toWorld, normal, centroid, edgePts, dist, sub, dot, dockSnapped, overlaps, measure, foldAngle, newId, bands, cap, flat, body, withIds } from '../core/geometry.js';
import { TYPE_UI, TYPE_ORDER, CONTROLLERS, PACKS, PRESETS, makePreset } from '../core/catalog.js';
import { planWiring } from '../core/wiring.js';
import { analyse, partsList, withStock, consume, toCsv } from '../core/bom.js';
import { lampFile, projectFile, readProject, pack, unpack, deviceUrl } from '../core/export.js';

const modules = await (await fetch(new URL('../data/lamp-modules.json', import.meta.url))).json();
const $ = s => document.querySelector(s);
const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
const fmt = (x, d = 0) => (x === Infinity ? '∞' : Number(x).toLocaleString('de-DE', { minimumFractionDigits: d, maximumFractionDigits: d }));
const store = {
  get(k, d) { try { const v = localStorage.getItem('lwcfg.' + k); return v == null ? d : JSON.parse(v); } catch { return d; } },
  set(k, v) { try { localStorage.setItem('lwcfg.' + k, JSON.stringify(v)); } catch { /* private mode */ } },
};

// ---------- state ----------
const OUT_COLORS = { A: '#e8a04a', B: '#5b8def', C: '#4cb87a', D: '#c86ad0' };
const SEAM_COLORS = { kette: '#ffffff', strom: '#e05a4f', masse: '#7a7f88' };
const S = {
  project: null, step: 'start', sel: -1, paintType: 'weiss', paintOut: 'A',
  ang: store.get('ang', 30), hover: null, hist: [], fut: [],
  stock: store.get('stock', {}), device: store.get('device', ''),
};
let D = null; // derived: topo, wiring, ana, parts

function setProject(p, { keepHist = false } = {}) {
  if (!keepHist) { S.hist = []; S.fut = []; }
  S.project = p; S.sel = -1; derive(); rebuild(); panel(); frame();
}
function edit(fn) {
  S.hist.push(JSON.stringify(S.project)); if (S.hist.length > 100) S.hist.shift(); S.fut = [];
  fn(S.project); derive(); rebuild(); panel();
}
function undo() { if (!S.hist.length) return; S.fut.push(JSON.stringify(S.project)); S.project = JSON.parse(S.hist.pop()); S.sel = -1; derive(); rebuild(); panel(); }
function redo() { if (!S.fut.length) return; S.hist.push(JSON.stringify(S.project)); S.project = JSON.parse(S.fut.pop()); S.sel = -1; derive(); rebuild(); panel(); }
function derive() {
  const p = S.project, outs = CONTROLLERS[p.controller].outputs;
  const topo = topology(p.tris), wiring = planWiring(p.tris, outs, topo);
  D = { topo, wiring, ana: analyse(p, wiring, modules), parts: partsList(p, wiring, modules) };
  store.set('project', projectFile(p));
}

// ---------- 3D ----------
const canvas = $('#c');
const renderer = new THREE.WebGLRenderer({ canvas, antialias: true, preserveDrawingBuffer: true });
renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
const scene = new THREE.Scene();
scene.background = new THREE.Color(0x20232a);
const camera = new THREE.PerspectiveCamera(40, 1, 1, 5000);
camera.up.set(0, 0, 1);
const controls = new OrbitControls(camera, canvas);
controls.enableDamping = true;
scene.add(new THREE.HemisphereLight(0xffffff, 0x404048, 1.6));
const sun = new THREE.DirectionalLight(0xffffff, 1.4); sun.position.set(200, -300, 400); scene.add(sun);
const grid = new THREE.GridHelper(400, 20, 0x3a3f48, 0x2c3038); grid.rotation.x = Math.PI / 2; scene.add(grid);
const root = new THREE.Group(); scene.add(root);
const ghostMat = new THREE.MeshBasicMaterial({ color: 0xffffff, transparent: true, opacity: 0.35, side: THREE.DoubleSide, depthWrite: false });
const ghost = new THREE.Mesh(new THREE.BufferGeometry(), ghostMat); ghost.visible = false; scene.add(ghost);
const ghostEdge = new THREE.LineSegments(new THREE.BufferGeometry(), new THREE.LineBasicMaterial({ color: 0xffffff })); ghostEdge.visible = false; scene.add(ghostEdge);
let boardMesh = null, faceTri = [];

// PCB outline (datasheet frame: base on y = 0, apex at y = 27.713), from the LED board thread.
const OUTLINE = [[-10.024, 0], [10.024, 0], [13.012, 5.175], [2.988, 22.538], [-2.988, 22.538], [-13.012, 5.175]];
const CY = 9.2376; // centroid height in the datasheet frame
const LED_COL = { wwa: '#fff0c8', rgb: '#bfe0ff', amb: '#ffae40' };

function triColor(t) {
  if (S.step === 'wire') return OUT_COLORS[t.out] || '#555';
  return TYPE_UI[t.type]?.color || '#888';
}
function rebuild() {
  root.clear();
  const p = S.project, pos = [], col = [], c = new THREE.Color();
  faceTri = [];
  const ledBoxes = [], inMarks = [];
  p.tris.forEach((t, i) => {
    const rot = D.wiring.per[i]?.rot ?? 0, f = boardFrame(t, rot), n = f.n;
    const P = OUTLINE.map(([x, y]) => toWorld(f, x, y));
    c.set(triColor(t)); if (i === S.sel) c.lerp(new THREE.Color('#ffffff'), 0.55);
    const back = P.map(q => q.map((x, k) => x - n[k] * 1.6));
    const push = (a, b, d, cc) => { for (const q of [a, b, d]) { pos.push(...q); col.push(cc.r, cc.g, cc.b); } faceTri.push(i); };
    const dark = c.clone().multiplyScalar(0.45);
    for (let k = 1; k < P.length - 1; k++) { push(P[0], P[k], P[k + 1], c); push(back[0], back[k + 1], back[k], dark); }
    for (let k = 0; k < P.length; k++) { const k2 = (k + 1) % P.length; push(P[k], back[k], back[k2], dark); push(P[k], back[k2], P[k2], dark); }
    // LEDs from the module type
    const type = modules.types[t.type];
    type?.pixels.forEach(px => {
      const first = px.ch.find(ch => ch.k);
      if (!first) return;
      const kind = px.ch.some(ch => ch.k === 'r') ? 'rgb' : px.ch.some(ch => ch.k === 'ww') ? 'wwa' : 'amb';
      const pts = px.ch.flatMap(ch => (!ch.at ? [] : Array.isArray(ch.at[0]) ? ch.at : [ch.at]));
      const uniq = pts.filter((q, j) => pts.findIndex(o => o[0] === q[0] && o[1] === q[1]) === j);
      uniq.forEach(([x, y]) => ledBoxes.push({ p: toWorld(f, x, y + CY), n, ex: f.ex, ey: f.ey, kind }));
    });
    if (S.step === 'wire') inMarks.push(f);
  });
  const g = new THREE.BufferGeometry();
  g.setAttribute('position', new THREE.Float32BufferAttribute(pos, 3));
  g.setAttribute('color', new THREE.Float32BufferAttribute(col, 3));
  g.computeVertexNormals();
  boardMesh = new THREE.Mesh(g, new THREE.MeshStandardMaterial({ vertexColors: true, roughness: 0.75, metalness: 0.05, side: THREE.DoubleSide }));
  root.add(boardMesh);

  // LEDs as small instanced boxes on the LED side
  if (ledBoxes.length) {
    const size = { wwa: 5, rgb: 2, amb: 1.4 };
    const im = new THREE.InstancedMesh(new THREE.BoxGeometry(1, 1, 1), new THREE.MeshBasicMaterial(), ledBoxes.length);
    const m = new THREE.Matrix4(), q = new THREE.Quaternion(), basis = new THREE.Matrix4();
    ledBoxes.forEach((L, k) => {
      basis.makeBasis(new THREE.Vector3(...L.ex), new THREE.Vector3(...L.ey), new THREE.Vector3(...L.n)); q.setFromRotationMatrix(basis);
      const s = size[L.kind];
      m.compose(new THREE.Vector3(...L.p.map((x, j) => x + L.n[j] * 0.6)), q, new THREE.Vector3(s, s, 1));
      im.setMatrixAt(k, m); im.setColorAt(k, new THREE.Color(LED_COL[L.kind]));
    });
    root.add(im);
  }

  // edges: free edges in the form step, seams by kind in the wiring step
  const segs = [], scol = [];
  const addSeg = (a, b, hex) => { const cc = new THREE.Color(hex); segs.push(...a, ...b); scol.push(cc.r, cc.g, cc.b, cc.r, cc.g, cc.b); };
  if (S.step === 'form') D.topo.free.forEach(({ i, k }) => { const [a, b] = edgePts(p.tris[i], k); addSeg(a, b, '#ffd27a'); });
  if (S.step === 'wire') D.wiring.seams.forEach(s => { const [a, b] = edgePts(p.tris[s.a], s.ka); addSeg(a, b, SEAM_COLORS[s.kind]); });
  if (segs.length) {
    const lg = new THREE.BufferGeometry();
    lg.setAttribute('position', new THREE.Float32BufferAttribute(segs, 3)); lg.setAttribute('color', new THREE.Float32BufferAttribute(scol, 3));
    root.add(new THREE.LineSegments(lg, new THREE.LineBasicMaterial({ vertexColors: true })));
  }
  if (S.step === 'wire') drawChains(inMarks);
  legend();
}
function drawChains(frames) {
  const p = S.project;
  // IN arrow on every board: points from the IN edge into the board
  const arr = [], acol = [], c = new THREE.Color();
  frames.forEach(f => {
    const off = q => q.map((x, k) => x + f.n[k] * 0.9);
    [[-3, 2.2], [3, 2.2], [0, 7.5]].forEach(([x, y]) => arr.push(...off(toWorld(f, x, y))));
    c.set('#ffffff'); for (let k = 0; k < 3; k++) acol.push(c.r, c.g, c.b);
  });
  const ag = new THREE.BufferGeometry();
  ag.setAttribute('position', new THREE.Float32BufferAttribute(arr, 3)); ag.setAttribute('color', new THREE.Float32BufferAttribute(acol, 3));
  root.add(new THREE.Mesh(ag, new THREE.MeshBasicMaterial({ vertexColors: true, side: THREE.DoubleSide })));
  // chain line through the centroids, lifted off the LED side; jumps dashed
  D.wiring.chains.forEach(ch => {
    const pts = ch.path.map(i => { const t = p.tris[i], n = normal(t); return new THREE.Vector3(...centroid(t).map((x, k) => x + n[k] * 2.5)); });
    const start = new THREE.Vector3(...D.wiring.controller);
    const line = new THREE.Line(new THREE.BufferGeometry().setFromPoints([start, ...pts]), new THREE.LineBasicMaterial({ color: OUT_COLORS[ch.id] }));
    root.add(line);
    ch.jumps.forEach(([a, b]) => {
      const g = new THREE.BufferGeometry().setFromPoints([pts[ch.path.indexOf(a)], pts[ch.path.indexOf(b)]]);
      const l = new THREE.Line(g, new THREE.LineDashedMaterial({ color: '#ff5050', dashSize: 3, gapSize: 2 })); l.computeLineDistances(); root.add(l);
    });
  });
  const cb = new THREE.Mesh(new THREE.CylinderGeometry(12, 12, 4, 24), new THREE.MeshStandardMaterial({ color: 0x222222 }));
  cb.rotation.x = Math.PI / 2; cb.position.set(...D.wiring.controller); root.add(cb);
}
function legend() {
  const L = $('#legend');
  if (S.step === 'wire') {
    L.innerHTML = CONTROLLERS[S.project.controller].outputs.map(o => `<span><i style="background:${OUT_COLORS[o]}"></i>Ausgang ${o}</span>`).join('')
      + Object.entries({ kette: 'Brücke Kette', strom: 'Brücke Strom', masse: 'Brücke Masse' }).map(([k, n]) => `<span><i style="background:${SEAM_COLORS[k]}"></i>${n}</span>`).join('')
      + '<span>▲ = IN-Kante</span>';
  } else {
    const used = new Set(S.project.tris.map(t => t.type));
    L.innerHTML = TYPE_ORDER.filter(k => used.has(k)).map(k => `<span><i style="background:${TYPE_UI[k].color}"></i>${esc(modules.types[k].name)}</span>`).join('')
      + (S.step === 'form' ? '<span><i style="background:#ffd27a"></i>freie Kante</span>' : '');
  }
}
function frame() {
  const m = measure(S.project.tris), P = S.project.tris.flatMap(t => t.v);
  const c = P.length ? P.reduce((s, v) => s.map((x, i) => x + v[i] / P.length), [0, 0, 0]) : [0, 0, 0];
  const r = Math.max(60, m.h, m.d, ...P.map(v => Math.abs(v[0]) * 2));
  controls.target.set(...c);
  camera.position.set(c[0] + r * 0.9, c[1] - r * 1.5, c[2] + r * 0.7);
  controls.update();
}
function resize() {
  const w = canvas.clientWidth, h = canvas.clientHeight;
  if (canvas.width !== Math.floor(w * renderer.getPixelRatio()) || canvas.height !== Math.floor(h * renderer.getPixelRatio())) {
    renderer.setSize(w, h, false); camera.aspect = w / Math.max(h, 1); camera.updateProjectionMatrix();
  }
}
renderer.setAnimationLoop(() => { resize(); controls.update(); renderer.render(scene, camera); });

// ---------- picking ----------
const ray = new THREE.Raycaster(), ndc = new THREE.Vector2();
function pick(ev) {
  const r = canvas.getBoundingClientRect();
  ndc.set(((ev.clientX - r.left) / r.width) * 2 - 1, -((ev.clientY - r.top) / r.height) * 2 + 1);
  ray.setFromCamera(ndc, camera);
  const hit = boardMesh ? ray.intersectObject(boardMesh)[0] : null;
  return hit ? { i: faceTri[hit.faceIndex], p: hit.point.toArray() } : null;
}
// nearest free edge of the hit triangle to the hit point
function hoverEdge(h) {
  if (!h) return null;
  const t = S.project.tris[h.i]; let best = null;
  for (const k of [0, 1, 2]) {
    if (!D.topo.free.some(f => f.i === h.i && f.k === k)) continue;
    const [a, b] = edgePts(t, k), ab = sub(b, a), s = Math.max(0, Math.min(1, dot(sub(h.p, a), ab) / dot(ab, ab)));
    const d = dist(h.p, a.map((x, j) => x + ab[j] * s));
    if (d < 11 && (!best || d < best.d)) best = { i: h.i, k, d };
  }
  return best;
}
function ghostTri() {
  const hv = S.hover; if (!hv) return null;
  const r = dockSnapped(S.project.tris, hv.i, hv.k, S.ang);
  return r;
}
function showGhost() {
  const r = S.step === 'form' ? ghostTri() : null;
  ghost.visible = ghostEdge.visible = !!r;
  if (!r) { hint(); return; }
  const v = r.tri.v.flat();
  ghost.geometry.dispose(); ghost.geometry = new THREE.BufferGeometry(); ghost.geometry.setAttribute('position', new THREE.Float32BufferAttribute(v, 3));
  const bad = overlaps(S.project.tris, r.tri);
  ghostMat.color.set(bad ? '#ff5050' : r.snapped ? '#5cff9a' : '#ffffff');
  ghostEdge.geometry.dispose(); ghostEdge.geometry = new THREE.BufferGeometry();
  const [a, b, c] = r.tri.v; ghostEdge.geometry.setAttribute('position', new THREE.Float32BufferAttribute([...a, ...b, ...b, ...c, ...c, ...a], 3));
  ghostEdge.material.color.set(ghostMat.color);
  const ang = r.snapped ? r.snapped.angle : S.ang;
  hint(bad ? 'Hier liegt schon ein Dreieck.' : `Knick ${fmt(ang, 1)}°${r.snapped ? ', rastet ein und schließt die Lücke' : ''}. Mausrad ändert den Winkel (Shift: 1°), Klick setzt.`);
}
function hint(s) {
  const H = {
    start: 'Vorlage oder Generator wählen. Ziehen dreht die Ansicht, Rechtsklick verschiebt, Mausrad zoomt.',
    form: 'Maus auf eine gelbe freie Kante: Geisterdreieck mit Knickwinkel. Klick auf ein Dreieck wählt es aus.',
    paint: 'Typ wählen, Klick färbt ein Dreieck, Doppelklick das ganze Band auf gleicher Höhe.',
    wire: 'Ausgang wählen, Klick ordnet ein Dreieck zu, Doppelklick das ganze Band. Die Kette plant sich selbst.',
    parts: 'Teileliste mit Lagerbestand.',
    upload: 'Lampe an LightWitch übergeben.',
  };
  $('#hint').textContent = s || H[S.step] || '';
}
let down = null, lastClick = 0;
canvas.addEventListener('pointerdown', e => { down = [e.clientX, e.clientY]; });
canvas.addEventListener('pointermove', e => {
  if (S.step !== 'form' || e.buttons) return;
  const hv = hoverEdge(pick(e));
  const same = hv && S.hover && hv.i === S.hover.i && hv.k === S.hover.k;
  if (!same) { S.hover = hv; controls.enableZoom = !hv; showGhost(); }
});
canvas.addEventListener('wheel', e => {
  if (S.step !== 'form' || !S.hover) return;
  e.preventDefault();
  S.ang = Math.max(-180, Math.min(180, Math.round(S.ang + Math.sign(e.deltaY) * (e.shiftKey ? 1 : 5))));
  store.set('ang', S.ang); showGhost(); syncAngle();
}, { passive: false });
canvas.addEventListener('pointerup', e => {
  if (!down || Math.hypot(e.clientX - down[0], e.clientY - down[1]) > 5) return;
  const dbl = Date.now() - lastClick < 350; lastClick = Date.now();
  const h = pick(e);
  if (S.step === 'form') {
    if (S.hover) return placeGhost();
    S.sel = h ? h.i : -1; rebuild(); panel(); return;
  }
  if (!h) { S.sel = -1; rebuild(); panel(); return; }
  if (S.step === 'paint' || S.step === 'wire') {
    const key = S.step === 'paint' ? 'type' : 'out', val = S.step === 'paint' ? S.paintType : S.paintOut;
    const targets = dbl ? bandOf(h.i) : [h.i];
    edit(p => targets.forEach(i => { p.tris[i][key] = val; if (key === 'out') clearLocks(p); }));
    return;
  }
  S.sel = h.i; rebuild(); panel();
});
// triangles of the same band: same centroid height and same tilt as the clicked one
function bandOf(i) {
  const t = S.project.tris[i], z = centroid(t)[2], nz = normal(t)[2];
  return S.project.tris.map((o, j) => (Math.abs(centroid(o)[2] - z) < 4 && Math.abs(normal(o)[2] - nz) < 0.15 ? j : -1)).filter(j => j >= 0);
}
function clearLocks(p) { p.tris.forEach(t => { delete t.pos; }); }
function placeGhost() {
  const r = ghostTri(); if (!r || overlaps(S.project.tris, r.tri)) return;
  edit(p => { r.tri.id = newId(p.tris); clearLocks(p); p.tris.push(r.tri); });
  S.hover = null; controls.enableZoom = true; showGhost();
}
window.addEventListener('keydown', e => {
  if (e.target.matches('input, select, textarea')) return;
  if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'z') { e.preventDefault(); e.shiftKey ? redo() : undo(); }
  else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'y') { e.preventDefault(); redo(); }
  else if ((e.key === 'Delete' || e.key === 'Backspace') && S.sel >= 0 && S.step === 'form') deleteSel();
});
function deleteSel() { const i = S.sel; if (i < 0) return; S.sel = -1; edit(p => { p.tris.splice(i, 1); clearLocks(p); }); }

// ---------- panels ----------
function syncAngle() { const a = $('#ang'), o = $('#angOut'); if (a) a.value = S.ang; if (o) o.textContent = S.ang + '°'; }
function setStep(s) {
  S.step = s; S.hover = null; controls.enableZoom = true;
  document.querySelectorAll('#steps button').forEach(b => b.classList.toggle('on', b.dataset.step === s));
  rebuild(); panel(); showGhost();
}
document.querySelectorAll('#steps button').forEach(b => b.addEventListener('click', () => setStep(b.dataset.step)));
$('#undo').onclick = undo; $('#redo').onclick = redo;

const GEN = store.get('gen', { n: 6, rows: 2, type: 'weiss', capTop: false, capBot: false, cols: 12, frows: 3, body: 'ico', gap: 0 });
function panel() {
  const P = $('#panel'), p = S.project;
  $('#undo').disabled = !S.hist.length; $('#redo').disabled = !S.fut.length;
  const h = {
    start: () => `
      <h2>Lampe beginnen</h2>
      <div class="row"><label>Name <input type="text" id="name" value="${esc(p.name)}"></label></div>
      <h3>Vorlagen aus dem Katalog</h3>
      <div class="cards">${Object.entries(PRESETS).map(([k, v]) => `<button class="card" data-preset="${k}"><b>${esc(v.name)}</b><small>${esc(v.sub)}</small></button>`).join('')}</div>
      <h3>Generator</h3>
      <p class="quiet">Ersetzt das Modell. Danach in „Form“ frei weiterbauen.</p>
      <div class="row"><label>Ecken <input type="number" id="gN" min="3" max="18" value="${GEN.n}"></label><label>Bänder <input type="number" id="gRows" min="1" max="12" value="${GEN.rows}"></label><span class="quiet">je Band 2 × Ecken Dreiecke</span></div>
      <div class="row"><label><input type="checkbox" id="gTop" ${GEN.capTop ? 'checked' : ''}> Kappe oben</label><label><input type="checkbox" id="gBot" ${GEN.capBot ? 'checked' : ''}> Kappe unten</label><span class="quiet">(n = 3 bis 6)</span></div>
      <div class="row"><button id="gBands">Ringband erzeugen</button></div>
      <div class="row">Fläche <input type="number" id="gCols" min="1" max="80" value="${GEN.cols}"> × <input type="number" id="gFRows" min="1" max="30" value="${GEN.frows}"> Dreiecke <button id="gFlat">Fläche erzeugen</button></div>
      <div class="row">Körper <select id="gBody">${[['tetra', 'Tetraeder (4)'], ['okt', 'Oktaeder (8)'], ['ico', 'Ikosaeder (20)']].map(([k, n]) => `<option value="${k}" ${GEN.body === k ? 'selected' : ''}>${n}</option>`).join('')}</select>
        Abstand zum Kern <input type="number" id="gGap" min="0" max="40" value="${GEN.gap}"> mm <button id="gBody2">Körper erzeugen</button></div>
      <h3>Projekt</h3>
      <div class="row"><button id="openFile">Projekt öffnen …</button><button id="saveFile">Projekt speichern</button><button id="shareLink">Link kopieren</button></div>
      <input type="file" id="file" accept=".json,application/json" hidden>
      <p id="msg" class="quiet"></p>`,
    form: () => {
      const t = p.tris[S.sel];
      let sel = '<p class="quiet">Kein Dreieck gewählt.</p>';
      if (t) {
        const nb = D.topo.nb[S.sel].map(x => `${p.tris[x.j].id} (${fmt(foldAngle(t, x.k, p.tris[x.j]), 1)}°)`).join(', ') || 'keine';
        sel = `<p><b>${esc(t.id)}</b>, ${esc(modules.types[t.type].name)}. Nachbarn: ${nb}</p>
          <div class="row"><button id="del">Löschen</button></div>`;
      }
      const used = [...new Set(D.topo.seams.map(s => Math.round(foldAngle(p.tris[s.a], s.ka, p.tris[s.b]))))].sort((a, b) => a - b).slice(0, 12);
      return `<h2>Form</h2>
        <p>Auf eine gelbe Kante zeigen, Winkel einstellen, klicken. Rastet die freie Ecke auf eine vorhandene Ecke ein, wird das Geisterdreieck grün und die Lücke schließt sich.</p>
        <div class="angle"><span>Knick</span><input type="range" id="ang" min="-180" max="180" step="1" value="${S.ang}"><b id="angOut">${S.ang}°</b></div>
        <div class="chips">${[0, 15, 30, 45, 60, 90, ...used].filter((x, i, a) => a.indexOf(x) === i).map(a => `<button data-ang="${a}">${a}°</button>`).join('')}</div>
        <p class="quiet">Positiv knickt nach hinten, weg von der LED-Seite. Neue Dreiecke übernehmen Typ und Ausgang vom Nachbarn.</p>
        <h3>Auswahl</h3>${sel}`;
    },
    paint: () => `<h2>Bestückung</h2>
      <div class="swatches">${TYPE_ORDER.map(k => `<button class="swatch ${S.paintType === k ? 'on' : ''}" data-type="${k}"><i style="background:${TYPE_UI[k].color}"></i>${esc(modules.types[k].name)}</button>`).join('')}</div>
      <p class="quiet">${esc(TYPE_UI[S.paintType].sub)}, ${modules.types[S.paintType].pixels.length} Pixel, ${fmt(typeLm(S.paintType), 1)} lm bei 100 %.</p>
      <h3>Ganze Lampe</h3><div class="row"><button id="allType">Alle Dreiecke auf ${esc(modules.types[S.paintType].name)}</button></div>`,
    wire: () => {
      const ctl = CONTROLLERS[p.controller], w = D.wiring;
      const rows = w.chains.map(c => { const o = D.ana.outs[c.id]; return `<tr><td><i style="display:inline-block;width:10px;height:10px;background:${OUT_COLORS[c.id]}"></i> ${c.id}</td><td class="n">${c.path.length}</td><td class="n">${o.pixels}</td><td class="n">${fmt(o.mA / 1000, 2)} A</td><td class="n">${c.jumps.length || ''}</td><td>${c.locked ? 'fest' : ''}</td></tr>`; }).join('');
      const t = p.tris[S.sel], pt = w.per[S.sel];
      return `<h2>Verkabelung</h2>
        <div class="row"><label>Steuerboard <select id="ctl">${Object.entries(CONTROLLERS).map(([k, c]) => `<option value="${k}" ${p.controller === k ? 'selected' : ''}>${c.name}</option>`).join('')}</select></label>
          <label>Akku <select id="pack">${Object.entries(PACKS).map(([k, c]) => `<option value="${k}" ${p.pack === k ? 'selected' : ''}>${c.name}</option>`).join('')}</select></label></div>
        <h3>Ausgang malen</h3>
        <div class="swatches">${ctl.outputs.map(o => `<button class="swatch ${S.paintOut === o ? 'on' : ''}" data-out="${o}"><i style="background:${OUT_COLORS[o]}"></i>Ausgang ${o}</button>`).join('')}</div>
        <h3>Ketten</h3>
        <table><tr><th>Ausgang</th><th class="n">Dreiecke</th><th class="n">Pixel</th><th class="n">Vollast</th><th class="n">Sprünge</th><th></th></tr>${rows}</table>
        <div class="row"><button id="lock">Kette festschreiben</button><button id="unlock">Neu planen</button></div>
        <p class="quiet">Festschreiben speichert die Reihenfolge im Projekt, damit eine gelötete Lampe ihre Kette behält. Jede Änderung an Form oder Ausgängen plant neu.</p>
        ${t ? `<h3>Auswahl</h3><p><b>${esc(t.id)}</b>: Ausgang ${pt.chain}, Platz ${pt.pos + 1}, IN-Kante ${pt.rot + 1}, Eingang über ${({ seam: 'Naht vom Vorgänger', cable: 'Kabel vom Steuerboard', jumper: 'Drahtbrücke' })[pt.inFrom]}</p>` : ''}`;
    },
    parts: () => {
      const rows = withStock(D.parts.rows, S.stock);
      return `<h2>Teileliste</h2>
        <table><tr><th>Teil</th><th class="n">Menge</th><th class="n">Lager</th><th class="n">Fehlt</th></tr>
        ${rows.map(r => `<tr><td>${esc(r.name)}</td><td class="n">${r.qty}</td><td class="n"><input type="number" min="0" data-stock="${r.key}" value="${r.have}"></td><td class="n ${r.missing ? 'miss' : ''}">${r.missing || '✓'}</td></tr>`).join('')}</table>
        <p class="quiet">${D.parts.pins} Pins Stiftleiste für die Brücken. Teile ca. ${fmt(D.parts.total, 2)} USD (Schätzpreise, ohne Schirm und Gehäuse).</p>
        <div class="row noprint"><button id="csv">CSV herunterladen</button><button id="print">Drucken</button><button id="consume">Bestand abziehen</button></div>
        <p class="quiet">Der Lagerbestand liegt nur in diesem Browser.</p>
        <div class="row noprint"><button id="stockSave">Lager sichern</button><button id="stockOpen">Lager laden …</button><input type="file" id="stockFile" accept=".json" hidden></div>`;
    },
    upload: () => `<h2>In LightWitch laden</h2>
      <p>Lampe zusammenlöten, Steuerboard anstecken, dann hier die Adresse der Lampe eintragen. Der Knopf öffnet die LightWitch-Oberfläche mit der Lampe im Link, dort noch „Übernehmen“ drücken.</p>
      <div class="row"><label>Adresse <input type="text" id="dev" placeholder="laterne.local oder 192.168.1.20" value="${esc(S.device)}"></label></div>
      <div class="row"><button class="primary" id="send">In LightWitch laden</button></div>
      <h3>Ersatzweg</h3>
      <div class="row"><button id="dl">Lampen-Datei herunterladen</button></div>
      <p class="quiet">Die Datei lässt sich in der LightWitch-Oberfläche hochladen. Format: lightwitch-lamp v1, ${D.wiring.chains.length} Ausgänge, ${D.ana.pixels} Pixel.</p>
      <p id="msg" class="quiet"></p>`,
  };
  P.innerHTML = h[S.step]();
  bind();
  stats();
}
const typeLm = k => modules.types[k].pixels.reduce((s, px) => s + px.ch.reduce((a, c) => a + (c.lm || 0), 0), 0);
function stats() {
  const a = D.ana, m = measure(S.project.tris), p = S.project;
  const counts = TYPE_ORDER.filter(k => a.counts[k]).map(k => `${a.counts[k]} ${esc(modules.types[k].name)}`).join(', ') || 'keine';
  const warn = [...a.warnings.map(w => ({ msg: w.msg, bad: true })), ...D.wiring.warnings.map(w => ({
    msg: w.code === 'jumps' ? `Ausgang ${w.out}: ${w.n} Drahtbrücke${w.n > 1 ? 'n' : ''}, keine durchgehende Kette über Nähte.` :
      w.code === 'split' ? `Ausgang ${w.out} besteht aus getrennten Teilen.` : `${w.tris.length} Dreiecke ohne Ausgang des ${CONTROLLERS[p.controller].name}.`,
    tri: w.tris?.[0],
  }))];
  $('#stats').innerHTML = `<dl class="kv">
    <dt>Dreiecke</dt><dd>${a.tris}: ${counts}</dd>
    <dt>Maße</dt><dd>${fmt(m.w)} × ${fmt(m.t)} × ${fmt(m.h)} mm (B × T × H), Ø ${fmt(m.d)} mm</dd>
    <dt>Pixel</dt><dd>${a.pixels}</dd>
    <dt>Vollast</dt><dd>${fmt(a.lm)} lm, ${fmt(a.mA / 1000, 1)} A, ${fmt(a.fullW, 1)} W</dd>
    <dt>Dauerbetrieb</dt><dd>${fmt(a.contLm)} lm bei ${fmt(a.contW, 1)} W (0,3 W je Dreieck)</dd>
    <dt>Laufzeit</dt><dd>${PACKS[p.pack].s ? `${fmt(a.hoursCont, 1)} h im Dauerbetrieb, ${fmt(a.hoursIdle)} h im Ruhezustand` : 'Netzteil'}</dd>
  </dl>${warn.map((w, i) => `<p class="warn ${w.bad ? 'bad' : ''}" data-w="${i}">⚠ ${esc(w.msg)}</p>`).join('')}`;
  document.querySelectorAll('#stats [data-w]').forEach(el => el.onclick = () => { const w = warn[+el.dataset.w]; if (w.tri != null) { S.sel = w.tri; rebuild(); panel(); } });
}
function bind() {
  const on = (sel, ev, fn) => document.querySelectorAll(sel).forEach(el => el.addEventListener(ev, fn));
  const p = S.project;
  on('[data-preset]', 'click', e => { setProject(makePreset(e.currentTarget.dataset.preset)); });
  on('#name', 'change', e => edit(q => { q.name = e.target.value || 'Lampe'; }));
  const gen = () => { GEN.n = +$('#gN').value; GEN.rows = +$('#gRows').value; GEN.capTop = $('#gTop').checked; GEN.capBot = $('#gBot').checked; GEN.cols = +$('#gCols').value; GEN.frows = +$('#gFRows').value; GEN.body = $('#gBody').value; GEN.gap = +$('#gGap').value; store.set('gen', GEN); };
  const replace = tris => { S.hist.push(JSON.stringify(S.project)); S.project = { ...S.project, tris: withIds(tris.map(t => ({ ...t, out: t.out || CONTROLLERS[S.project.controller].outputs[0] }))) }; S.sel = -1; derive(); rebuild(); panel(); frame(); };
  on('#gBands', 'click', () => {
    gen(); const n = Math.max(3, Math.min(18, GEN.n)), b = bands(n, Math.max(1, GEN.rows), { types: [S.paintType] });
    let tris = b.tris;
    if (GEN.capTop && n <= 6) tris = [...tris, ...cap(b.top, 1, S.paintType)];
    if (GEN.capBot && n <= 6) tris = [...tris, ...cap(b.bottom, -1, S.paintType)];
    const z0 = Math.min(...tris.flatMap(t => t.v.map(q => q[2])));
    replace(tris.map(t => ({ ...t, v: t.v.map(q => [q[0], q[1], +(q[2] - z0).toFixed(3)]) })));
  });
  on('#gFlat', 'click', () => { gen(); replace(flat(Math.max(1, GEN.cols), Math.max(1, GEN.frows), { type: S.paintType })); });
  on('#gBody2', 'click', () => { gen(); const base = { tetra: 32, okt: 32, ico: 32 }[GEN.body]; const a = base + GEN.gap * ({ tetra: 4.9, okt: 2.45, ico: 1.32 }[GEN.body]); replace(body(GEN.body, a, { type: S.paintType })); });
  on('#saveFile', 'click', () => download(fileName(p.name) + '.lampe.json', JSON.stringify(projectFile(p), null, 1)));
  on('#openFile', 'click', () => $('#file').click());
  on('#file', 'change', async e => { const f = e.target.files[0]; if (!f) return; try { setProject(readProject(JSON.parse(await f.text()))); } catch (err) { $('#msg').textContent = err.message; } });
  on('#shareLink', 'click', async () => { const url = location.href.split('#')[0] + '#p=' + await pack(projectFile(p)); try { await navigator.clipboard.writeText(url); $('#msg').textContent = 'Link kopiert (' + fmt(url.length) + ' Zeichen).'; } catch { $('#msg').textContent = url; } });
  on('#ang', 'input', e => { S.ang = +e.target.value; store.set('ang', S.ang); syncAngle(); });
  on('[data-ang]', 'click', e => { S.ang = +e.currentTarget.dataset.ang; store.set('ang', S.ang); syncAngle(); });
  on('#del', 'click', deleteSel);
  on('[data-type]', 'click', e => { S.paintType = e.currentTarget.dataset.type; panel(); });
  on('#allType', 'click', () => edit(q => q.tris.forEach(t => { t.type = S.paintType; })));
  on('[data-out]', 'click', e => { S.paintOut = e.currentTarget.dataset.out; panel(); });
  on('#ctl', 'change', e => edit(q => { q.controller = e.target.value; const outs = CONTROLLERS[q.controller].outputs; q.tris.forEach(t => { if (!outs.includes(t.out)) t.out = outs[outs.length - 1]; }); clearLocks(q); if (CONTROLLERS[q.controller].cells !== PACKS[q.pack].s && PACKS[q.pack].s) q.pack = q.controller === 'S' ? '1s4p' : '2s2p'; }));
  on('#pack', 'change', e => edit(q => { q.pack = e.target.value; }));
  on('#lock', 'click', () => edit(q => D.wiring.chains.forEach(c => c.path.forEach((ti, k) => { q.tris[ti].pos = k; }))));
  on('#unlock', 'click', () => edit(clearLocks));
  on('[data-stock]', 'change', e => { S.stock[e.target.dataset.stock] = Math.max(0, +e.target.value || 0); store.set('stock', S.stock); panel(); });
  on('#csv', 'click', () => download(fileName(p.name) + '-teile.csv', '﻿' + toCsv(D.parts.rows, S.stock), 'text/csv'));
  on('#print', 'click', () => window.print());
  on('#consume', 'click', () => { S.stock = consume(D.parts.rows, S.stock); store.set('stock', S.stock); panel(); });
  on('#stockSave', 'click', () => download('lager.json', JSON.stringify(S.stock, null, 1)));
  on('#stockOpen', 'click', () => $('#stockFile').click());
  on('#stockFile', 'change', async e => { const f = e.target.files[0]; if (!f) return; try { S.stock = JSON.parse(await f.text()); store.set('stock', S.stock); panel(); } catch { /* not a stock file */ } });
  on('#dev', 'change', e => { S.device = e.target.value.trim(); store.set('device', S.device); });
  on('#dl', 'click', () => download(fileName(p.name) + '.lightwitch-lamp.json', JSON.stringify(lampFile(p, D.wiring, modules))));
  on('#send', 'click', async () => {
    S.device = $('#dev').value.trim(); store.set('device', S.device);
    const base = deviceUrl(S.device);
    if (!base) { $('#msg').textContent = 'Bitte zuerst die Adresse der Lampe eintragen.'; return; }
    const win = window.open('about:blank', '_blank'); // open before the async step, or the popup blocker stops it
    const url = base + '#lamp=' + await pack(lampFile(p, D.wiring, modules));
    if (win) { win.location.href = url; $('#msg').textContent = `LightWitch geöffnet (${fmt(url.length)} Zeichen im Link). Dort „Übernehmen“ drücken.`; }
    else $('#msg').innerHTML = `Der Browser hat den Tab blockiert. <a href="${esc(url)}" target="_blank" rel="noopener">LightWitch öffnen</a>`;
  });
}
const fileName = s => String(s || 'lampe').toLowerCase().replace(/[^a-z0-9äöüß]+/g, '-').replace(/^-|-$/g, '') || 'lampe';
function download(name, text, type = 'application/json') {
  const a = document.createElement('a'); a.href = URL.createObjectURL(new Blob([text], { type })); a.download = name; a.click(); setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

// ---------- start ----------
async function initial() {
  const m = location.hash.match(/#p=([\w-]+)/);
  if (m) { try { return readProject(await unpack(m[1])); } catch { /* fall back */ } }
  try { const s = store.get('project', null); if (s) return readProject(s); } catch { /* fall back */ }
  return makePreset('allrounder');
}
setProject(await initial());
setStep('start');
window.__cfg = { screenOf: p => { const v = new THREE.Vector3(...p).project(camera), r = canvas.getBoundingClientRect(); return [r.left + (v.x + 1) / 2 * r.width, r.top + (1 - v.y) / 2 * r.height]; }, S, get D() { return D; }, setStep, setProject, makePreset, edit, camera, controls }; // for tests
