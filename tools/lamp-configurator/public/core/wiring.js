// Data chain planning. Every board has one IN edge (datasheet base) and two OUT edges on the
// same net; only one OUT is continued. A chain per output is therefore a path through
// neighbouring triangles that visits every triangle of the output once. Each board is rotated
// so its IN edge faces its predecessor. Where no path exists (or triangles do not touch, as in
// the balls) the chain continues over a 3-wire jumper.
import { topology, centroid, edgePts, mid, dist } from './geometry.js';

const STEP_BUDGET = 20000;

// Controller position: centre of the foot, or the body centre for closed bodies without a foot.
export function controllerPoint(tris) {
  if (!tris.length) return [0, 0, 0];
  const zs = tris.flatMap(t => t.v.map(p => p[2])), zmin = Math.min(...zs);
  return [0, 0, zmin];
}

function hamiltonPath(nodes, nbOf, start, order) {
  const inSet = new Set(nodes), N = nodes.length, path = [start], seen = new Set([start]);
  let steps = 0;
  const free = i => nbOf(i).filter(j => inSet.has(j) && !seen.has(j));
  const go = () => {
    if (path.length === N) return true;
    if (++steps > STEP_BUDGET) return false;
    const cur = path[path.length - 1];
    const cand = free(cur).sort((a, b) => free(a).length - free(b).length || order(a) - order(b));
    for (const j of cand) {
      path.push(j); seen.add(j);
      if (go()) return true;
      path.pop(); seen.delete(j);
      if (steps > STEP_BUDGET) return false;
    }
    return false;
  };
  return go() ? path : null;
}
// Greedy path with jumps: follow neighbours (fewest onward options first), jump to the nearest
// unvisited triangle when stuck.
function greedyPath(nodes, nbOf, start, C, order) {
  const inSet = new Set(nodes), seen = new Set([start]), path = [start], jumps = [];
  const free = i => nbOf(i).filter(j => inSet.has(j) && !seen.has(j));
  while (path.length < nodes.length) {
    const cur = path[path.length - 1];
    let next = free(cur).sort((a, b) => free(a).length - free(b).length || order(a) - order(b))[0];
    if (next == null) {
      next = nodes.filter(j => !seen.has(j)).sort((a, b) => dist(C[cur], C[a]) - dist(C[cur], C[b]) || order(a) - order(b))[0];
      jumps.push([cur, next]);
    }
    path.push(next); seen.add(next);
  }
  return { path, jumps };
}

// Plan all chains. Returns per output { id, path: [triIndex], jumps: [[from,to]] },
// per triangle { rot, chain, pos }, seam classes and warnings.
export function planWiring(tris, outputs, topo = topology(tris)) {
  const C = tris.map(centroid), ctl = controllerPoint(tris);
  const nbOf = i => topo.nb[i].map(x => x.j);
  const order = i => C[i][2] * 1000 + Math.atan2(C[i][1], C[i][0]);
  const per = tris.map(() => ({ rot: 0, chain: null, pos: -1, inFrom: null }));
  const chains = [], warnings = [];

  for (const id of outputs) {
    const nodes = tris.map((t, i) => (t.out === id ? i : -1)).filter(i => i >= 0);
    if (!nodes.length) continue;
    // starts: lowest and closest to the controller first
    // start candidates: closest to the controller first, then low-degree corners
    const deg = i => nbOf(i).filter(j => tris[j].out === id).length;
    const near = nodes.slice().sort((a, b) => dist(C[a], ctl) - dist(C[b], ctl));
    const starts = [...new Set([...near.slice(0, 6), ...near.filter(i => deg(i) <= 2).slice(0, 12)])];
    let path = null, jumps = [];
    // a locked chain (t.pos stored in the project) is kept as soldered
    const locked = nodes.every(i => Number.isInteger(tris[i].pos)) && new Set(nodes.map(i => tris[i].pos)).size === nodes.length;
    if (locked) {
      path = nodes.slice().sort((a, b) => tris[a].pos - tris[b].pos);
      path.forEach((ti, k) => { if (k && !topo.nb[ti].some(x => x.j === path[k - 1])) jumps.push([path[k - 1], ti]); });
    }
    if (!path) for (const s of starts) { path = hamiltonPath(nodes, nbOf, s, order); if (path) break; }
    if (!path) ({ path, jumps } = greedyPath(nodes, nbOf, starts[0], C, order));
    chains.push({ id, path, jumps, locked });
    if (jumps.length && nodes.some(i => topo.nb[i].some(x => tris[x.j].out === id)))
      warnings.push({ code: 'jumps', out: id, n: jumps.length, tris: jumps.map(j => j[1]) });
    const loose = nodes.every(i => !topo.nb[i].some(x => tris[x.j].out === id));
    if (!loose && !connected(nodes, nbOf)) warnings.push({ code: 'split', out: id, tris: nodes });
    path.forEach((ti, pos) => {
      const p = per[ti]; p.chain = id; p.pos = pos;
      const prev = pos ? path[pos - 1] : null;
      const viaSeam = prev != null ? topo.nb[ti].find(x => x.j === prev) : null;
      if (viaSeam) { p.rot = viaSeam.k; p.inFrom = 'seam'; }
      else { // cable from the controller or a jumper: IN edge closest to where the wire comes from
        const src = prev == null ? ctl : C[prev];
        p.rot = [0, 1, 2].sort((a, b) => dist(mid(...edgePts(tris[ti], a)), src) - dist(mid(...edgePts(tris[ti], b)), src))[0];
        p.inFrom = prev == null ? 'cable' : 'jumper';
      }
    });
  }
  const unassigned = tris.map((t, i) => (outputs.includes(t.out) ? -1 : i)).filter(i => i >= 0);
  if (unassigned.length) warnings.push({ code: 'unassigned', tris: unassigned });

  // seam classes
  const seams = topo.seams.map(s => {
    const A = tris[s.a], B = tris[s.b];
    if (A.out !== B.out) return { ...s, kind: 'masse' };
    const pa = per[s.a], pb = per[s.b];
    const chain = (pa.pos === pb.pos + 1 && pa.rot === s.ka && pa.inFrom === 'seam') || (pb.pos === pa.pos + 1 && pb.rot === s.kb && pb.inFrom === 'seam');
    return { ...s, kind: chain ? 'kette' : 'strom' };
  });
  return { chains, per, seams, warnings, controller: ctl };
}

function connected(nodes, nbOf) {
  if (nodes.length < 2) return true;
  const inSet = new Set(nodes), seen = new Set([nodes[0]]), st = [nodes[0]];
  while (st.length) { const i = st.pop(); for (const j of nbOf(i)) if (inSet.has(j) && !seen.has(j)) { seen.add(j); st.push(j); } }
  return seen.size === nodes.length;
}
