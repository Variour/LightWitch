// Pure, DOM-free core of the dock editor (interaction concept v1, §11: the
// DockResolver as pure functions over the grid model) — shared between the
// editor page (loaded as an ES module) and node:test unit tests. Mock-only
// for now; the M2 engine adopts this registry as its node registry, which is
// what keeps editor and engine from drifting.

// Wire/port types (concept §1.3): shape is the primary code.
// event ▶ · switch ■ · value ● · color ◉
export const SIGNAL_SHAPES = { event: '▶', switch: '■', value: '●', color: '◉' };

// Node registry: every node type with its fixed connector rows (PORT-01).
// Each row may carry one input port (left edge of the node) and/or one output
// port (right edge) — flow runs left to right, so a node's outputs meet the
// inputs of the node in the next column. Row order is fixed (nodes are
// static). `repeat: true` marks a
// repeatable connector (MULTI-01): the node can grow extra rows for it —
// repeatable inputs collect several sources, repeatable outputs feed several
// neighbors (this is the concept's fan-out mechanism). Repeatable ports must
// sit alone on their registry row so instance rows can be inserted below.
// Port shapes are still speculative until the M2 engine pins them down.
export const NODE_TYPES = {
  'button':       { cat: 'in',    rows: [{ out: { id: 'pressed', type: 'event', repeat: true } }] },
  'timer':        { cat: 'in',    rows: [{ in: { id: 'start', type: 'event' }, out: { id: 'elapsed', type: 'event' } },
                                         { in: { id: 'stop', type: 'event' } }] },
  'mesh-receive': { cat: 'in',    rows: [{ out: { id: 'payload', type: 'value', repeat: true } }] },
  'constant':     { cat: 'in',    rows: [{ out: { id: 'value', type: 'value', repeat: true } }] },
  'compare':      { cat: 'logic', rows: [{ in: { id: 'value', type: 'value' }, out: { id: 'isTrue', type: 'switch' } },
                                         { out: { id: 'isFalse', type: 'switch' } }] },
  'range':        { cat: 'logic', rows: [{ in: { id: 'value', type: 'value' }, out: { id: 'inRange', type: 'switch' } },
                                         { out: { id: 'outOfRange', type: 'switch' } }] },
  'gate':         { cat: 'logic', rows: [{ in: { id: 'value', type: 'value' }, out: { id: 'value', type: 'value' } },
                                         { in: { id: 'open', type: 'switch' } }] },
  'wait':         { cat: 'logic', rows: [{ in: { id: 'start', type: 'event' }, out: { id: 'done', type: 'event' } }] },
  'action':       { cat: 'out',   rows: [{ in: { id: 'trigger', type: 'event', repeat: true } }] },
  'mesh-send':    { cat: 'out',   rows: [{ in: { id: 'value', type: 'value', repeat: true } }] },
  'log':          { cat: 'out',   rows: [{ in: { id: 'value', type: 'value', repeat: true } }] },
};

export const CATEGORIES = { in: 'Sources', logic: 'Logic', out: 'Sinks' };

// Merge rules for multi-row value inputs (MULTI-02); event rows are always OR.
export const MULTI_RULES = ['max', 'sum', 'mean', 'last'];

// Adapter matrix (DOCK-04): which type conversions dock as a two-colored adapter// port, with which default rule. PROVISIONAL — the authoritative matrix lives
// in the system concept §5.3, which is not in this repo yet; replace this
// table when it lands. Key: `${outType}>${inType}`.
export const ADAPTER_MATRIX = {
  'value>switch': 'threshold 50%',
  'switch>value': '0 / 100%',
  'value>color':  'color ramp',
  'event>switch': 'pulse',
};

// Default chip values for free in-ports (PORT-03): percent for values,
// on/off for switches; event ports carry no chip.
export function defaultChip(type) {
  if (type === 'value') return 50;
  if (type === 'switch') return 0;
  return null;
}

// ── Geometry ─────────────────────────────────────────────────────────────────

// Extra instance rows of a repeatable port: node.rep = { 'in:trigger': 1, … }.
export function extraRows(node, dir, portId) {
  return (node.rep && node.rep[`${dir}:${portId}`]) || 0;
}

// A node occupies rows [row .. row + height - 1]: header + connector rows,
// where repeatable connectors contribute 1 + their extra rows.
export function nodeHeight(node) {
  const type = typeof node === 'string' ? node : node.type;
  const t = NODE_TYPES[type];
  if (!t) return 1;
  let h = 1;
  for (const r of t.rows) {
    let rows = 1;
    if (typeof node === 'object') {
      if (r.in && r.in.repeat) rows += extraRows(node, 'in', r.in.id);
      if (r.out && r.out.repeat) rows += extraRows(node, 'out', r.out.id);
    }
    h += rows;
  }
  return h;
}

// All port instances of a node with their global grid rows:
// [{dir, id, type, repeat, inst, row}] — inst 0 is the base row (MULTI-01).
export function portsOf(node) {
  const t = NODE_TYPES[node.type];
  if (!t) return [];
  const out = [];
  let row = node.row + 1;
  for (const r of t.rows) {
    const emit = (port, dir) => {
      const count = 1 + (port.repeat ? extraRows(node, dir, port.id) : 0);
      for (let inst = 0; inst < count; inst++)
        out.push({ dir, id: port.id, type: port.type, repeat: !!port.repeat, inst, row: row + inst });
      return count;
    };
    let used = 1;
    if (r.in) used = Math.max(used, emit(r.in, 'in'));
    if (r.out) used = Math.max(used, emit(r.out, 'out'));
    row += used;
  }
  return out;
}

export function overlaps(a, b) {
  if (a.col !== b.col) return false;
  const aEnd = a.row + nodeHeight(a) - 1;
  const bEnd = b.row + nodeHeight(b) - 1;
  return a.row <= bEnd && b.row <= aEnd;
}

// True if `node` can sit at (col,row) without overlapping any other node.
export function placementFree(doc, node, col, row) {
  const probe = { ...node, col, row };
  return !doc.nodes.some(o => o.id !== node.id && overlaps(probe, o));
}

// ── Compatibility & candidate search (DOCK-03/04) ────────────────────────────

// null = incompatible; 'direct' = same shape; otherwise the adapter rule name.
export function compat(outType, inType) {
  if (outType === inType) return 'direct';
  return ADAPTER_MATRIX[`${outType}>${inType}`] || null;
}

// How many links already use a given (node, dir, port).
export function portLoad(doc, nodeId, dir, portId) {
  return doc.links.filter(e => dir === 'out'
    ? e[0] === nodeId && e[1] === portId
    : e[2] === nodeId && e[3] === portId).length;
}

// A port's connection capacity = its instance-row count.
export function portCapacity(node, dir, portId) {
  const t = NODE_TYPES[node.type];
  if (!t) return 0;
  for (const r of t.rows) {
    const p = dir === 'in' ? r.in : r.out;
    if (p && p.id === portId) return 1 + (p.repeat ? extraRows(node, dir, portId) : 0);
  }
  return 0;
}

// Candidate search for a node hypothetically placed at (col,row): for each
// of its port instances, compatible counter-port instances in the adjacent
// column with a row distance ≤ 1. Priority: exact row > ±1 · direct > adapter
// (DOCK-03). Returns candidates sorted best-first.
export function dockCandidates(doc, node, col, row) {
  const probe = { ...node, col, row };
  const found = [];
  for (const my of portsOf(probe)) {
    const otherCol = my.dir === 'out' ? col + 1 : col - 1;
    for (const other of doc.nodes) {
      if (other.id === node.id || other.col !== otherCol) continue;
      for (const their of portsOf(other)) {
        if (their.dir === my.dir) continue;
        const dRow = Math.abs(my.row - their.row);
        if (dRow > 1) continue;
        const rule = my.dir === 'out' ? compat(my.type, their.type) : compat(their.type, my.type);
        if (!rule) continue;
        const link = my.dir === 'out'
          ? [probe.id, my.id, other.id, their.id]
          : [other.id, their.id, probe.id, my.id];
        found.push({ link, rule, dRow, myPort: my, theirPort: their, other });
      }
    }
  }
  found.sort((a, b) =>
    (a.dRow - b.dRow) ||
    ((a.rule === 'direct' ? 0 : 1) - (b.rule === 'direct' ? 0 : 1)));
  return found;
}

// True if an exact-row, adjacent-column port pairing exists that is NOT
// compatible — the transient red "does not dock" state (DOCK-04).
export function hasRejectedDock(doc, node, col, row) {
  const probe = { ...node, col, row };
  for (const my of portsOf(probe)) {
    const otherCol = my.dir === 'out' ? col + 1 : col - 1;
    for (const other of doc.nodes) {
      if (other.id === node.id || other.col !== otherCol) continue;
      for (const their of portsOf(other)) {
        if (their.dir === my.dir || their.row !== my.row) continue;
        const rule = my.dir === 'out' ? compat(my.type, their.type) : compat(their.type, my.type);
        if (!rule) return true;
      }
    }
  }
  return false;
}

// ── Link maintenance (INV-01, DOCK-05/07) ────────────────────────────────────

// A link is geometrically valid when some instance pairing of its two ports
// sits in adjacent columns with row distance ≤ 1 and the types dock. Rails
// (P1) will add the second legal representation per INV-01.
export function linkValid(doc, link) {
  const from = doc.nodes.find(n => n.id === link[0]);
  const to = doc.nodes.find(n => n.id === link[2]);
  if (!from || !to) return false;
  if (to.col !== from.col + 1) return false;
  const outs = portsOf(from).filter(p => p.dir === 'out' && p.id === link[1]);
  const ins = portsOf(to).filter(p => p.dir === 'in' && p.id === link[3]);
  if (!outs.length || !ins.length) return false;
  if (!compat(outs[0].type, ins[0].type)) return false;
  return outs.some(o => ins.some(i => Math.abs(o.row - i.row) <= 1));
}

// The adapter rule a link docks with ('direct' or a ADAPTER_MATRIX rule).
export function linkRule(doc, link) {
  const from = doc.nodes.find(n => n.id === link[0]);
  const to = doc.nodes.find(n => n.id === link[2]);
  if (!from || !to) return null;
  const outPort = portsOf(from).find(p => p.dir === 'out' && p.id === link[1]);
  const inPort = portsOf(to).find(p => p.dir === 'in' && p.id === link[3]);
  if (!outPort || !inPort) return null;
  return compat(outPort.type, inPort.type);
}

// After a node moved (or was added): drop links that no longer hold
// geometrically, then dock every port of the moved node with free capacity
// to its best candidates. Mutates doc.links; returns {dropped, added}.
export function settleLinks(doc, movedId) {
  const before = doc.links.length;
  doc.links = doc.links.filter(e => linkValid(doc, e));
  const dropped = before - doc.links.length;

  const node = doc.nodes.find(n => n.id === movedId);
  let added = 0;
  if (node) {
    for (const cand of dockCandidates(doc, node, node.col, node.row)) {
      const [fromId, outId, toId, inId] = cand.link;
      if (doc.links.some(e => e[0] === fromId && e[1] === outId && e[2] === toId && e[3] === inId))
        continue;
      const fromNode = doc.nodes.find(n => n.id === fromId);
      const toNode = doc.nodes.find(n => n.id === toId);
      if (portLoad(doc, fromId, 'out', outId) >= portCapacity(fromNode, 'out', outId)) continue;
      if (portLoad(doc, toId, 'in', inId) >= portCapacity(toNode, 'in', inId)) continue;
      doc.links.push(cand.link);
      added++;
    }
  }
  return { dropped, added };
}

// ── Multi-row helpers (MULTI-01/02) ──────────────────────────────────────────

// Grow/shrink a repeatable port by one row. Shrinking below the number of
// connected links (or below one row) is refused. Returns true on change.
export function setPortRows(doc, node, dir, portId, delta) {
  const cap = portCapacity(node, dir, portId);
  if (!cap) return false;
  const t = NODE_TYPES[node.type];
  const port = t.rows.map(r => (dir === 'in' ? r.in : r.out)).find(p => p && p.id === portId);
  if (!port || !port.repeat) return false;
  const next = cap + delta;
  if (next < 1 || next < portLoad(doc, node.id, dir, portId)) return false;
  node.rep = node.rep || {};
  node.rep[`${dir}:${portId}`] = next - 1;
  // Growing/shrinking may break nodes below — the caller re-settles; here we
  // only refuse a shrink that would orphan connected rows (checked above).
  return true;
}

// The merge rule of a multi-row value input (MULTI-02), default 'max'.
export function multiRule(node, portId) {
  return (node.cfg && node.cfg.rules && node.cfg.rules[portId]) || MULTI_RULES[0];
}

// ── Document validation (VAL-02, structural) ─────────────────────────────────

export function validateDoc(doc) {
  const errors = [];
  if (doc.v !== 1) errors.push('schema version must be 1');
  if (!Array.isArray(doc.nodes)) errors.push('nodes must be an array');
  if (!Array.isArray(doc.links)) errors.push('links must be an array');
  if (errors.length) return errors;
  const ids = new Set();
  for (const n of doc.nodes) {
    if (ids.has(n.id)) errors.push(`duplicate node id ${n.id}`);
    ids.add(n.id);
    if (!NODE_TYPES[n.type]) errors.push(`unknown node type "${n.type}"`);
  }
  for (const n of doc.nodes)
    for (const o of doc.nodes)
      if (n.id < o.id && overlaps(n, o)) errors.push(`nodes #${n.id} and #${o.id} overlap`);
  for (const e of doc.links)
    if (!linkValid(doc, e)) errors.push(`link ${JSON.stringify(e)} is not dockable`);
  for (const n of doc.nodes) {
    if (!NODE_TYPES[n.type]) continue;
    for (const dir of ['in', 'out'])
      for (const r of NODE_TYPES[n.type].rows) {
        const p = dir === 'in' ? r.in : r.out;
        if (p && portLoad(doc, n.id, dir, p.id) > portCapacity(n, dir, p.id))
          errors.push(`port ${n.id}:${dir}:${p.id} exceeds its row capacity`);
      }
  }
  return errors;
}
