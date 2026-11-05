import { test, describe } from 'node:test';
import assert from 'node:assert/strict';
import {
  NODE_TYPES, compat, dockCandidates, hasRejectedDock, settleLinks,
  linkValid, linkRule, nodeHeight, placementFree, validateDoc,
  portCapacity, portLoad, setPortRows, portsOf,
} from './graphs-core.js';

const doc = (nodes, links = []) => ({ v: 1, name: 't', nodes, links, notes: [] });

describe('registry & geometry', () => {
  test('node height = header + connector rows, repeat rows grow it', () => {
    assert.equal(nodeHeight('button'), 2);
    assert.equal(nodeHeight('timer'), 3);
    assert.equal(nodeHeight('range'), 3);
    const grown = { type: 'action', rep: { 'in:trigger': 2 } };
    assert.equal(nodeHeight(grown), 4);
  });

  test('portsOf emits one instance per repeat row with consecutive rows', () => {
    const n = { id: 1, type: 'action', col: 1, row: 1, rep: { 'in:trigger': 1 } };
    const ports = portsOf(n).filter(p => p.id === 'trigger');
    assert.equal(ports.length, 2);
    assert.deepEqual(ports.map(p => p.row), [3, 4].map(r => r - 1));  // rows 2 and 3
  });

  test('placement rejects overlap in the same column only', () => {
    const d = doc([{ id: 1, type: 'timer', col: 2, row: 1 }]);
    const probe = { id: 2, type: 'button' };
    assert.equal(placementFree(d, probe, 2, 2), false);  // rows 2-3 vs timer 1-3
    assert.equal(placementFree(d, probe, 2, 4), true);
    assert.equal(placementFree(d, probe, 3, 2), true);   // other column
  });
});

describe('compatibility (DOCK-04)', () => {
  test('same shape docks directly, defined pairs dock as adapter, others not', () => {
    assert.equal(compat('event', 'event'), 'direct');
    assert.equal(compat('value', 'switch'), 'threshold 50%');
    assert.equal(compat('event', 'color'), null);
  });
});

describe('candidate search (DOCK-03)', () => {
  test('finds an exact-row direct candidate first', () => {
    const d = doc([
      { id: 1, type: 'button', col: 1, row: 1 },  // pressed out at row 2
      { id: 2, type: 'wait', col: 2, row: 1 },    // start in at row 2
    ]);
    const c = dockCandidates(d, d.nodes[0], 1, 1);
    assert.ok(c.length >= 1);
    assert.deepEqual(c[0].link, [1, 'pressed', 2, 'start']);
    assert.equal(c[0].dRow, 0);
    assert.equal(c[0].rule, 'direct');
  });

  test('±1 row still qualifies, larger distances do not', () => {
    const near = doc([
      { id: 1, type: 'button', col: 1, row: 1 },
      { id: 2, type: 'wait', col: 2, row: 2 },    // start at row 3 → dRow 1
    ]);
    assert.equal(dockCandidates(near, near.nodes[0], 1, 1)[0].dRow, 1);
    const far = doc([
      { id: 1, type: 'button', col: 1, row: 1 },
      { id: 2, type: 'wait', col: 2, row: 3 },    // start at row 5 → dRow 3
    ]);
    assert.equal(dockCandidates(far, far.nodes[0], 1, 1).length, 0);
  });

  test('incompatible exact-row pairing reports the rejected state', () => {
    const d = doc([
      { id: 1, type: 'button', col: 1, row: 1 },     // pressed (event) out at row 2
      { id: 2, type: 'mesh-send', col: 2, row: 1 },  // value (value) in at row 2
    ]);
    assert.equal(hasRejectedDock(d, d.nodes[0], 1, 1), true);
    assert.equal(dockCandidates(d, d.nodes[0], 1, 1).length, 0);
  });
});

describe('link settling (DOCK-05/07, INV-01)', () => {
  test('moving a node next to a partner docks it; moving away undocks', () => {
    const d = doc([
      { id: 1, type: 'button', col: 1, row: 1 },
      { id: 2, type: 'wait', col: 4, row: 1 },
    ]);
    assert.deepEqual(settleLinks(d, 2), { dropped: 0, added: 0 });
    d.nodes[1].col = 2;
    const r = settleLinks(d, 2);
    assert.equal(r.added, 1);
    assert.deepEqual(d.links, [[1, 'pressed', 2, 'start']]);
    assert.equal(linkRule(d, d.links[0]), 'direct');
    d.nodes[1].col = 4;
    assert.equal(settleLinks(d, 2).dropped, 1);
    assert.deepEqual(d.links, []);
  });

  test('an adapter link validates and reports its rule', () => {
    const d = doc([
      { id: 1, type: 'timer', col: 1, row: 1 },  // elapsed out at row 2
      { id: 2, type: 'gate', col: 2, row: 0 },   // open in at row 2
    ], [[1, 'elapsed', 2, 'open']]);
    assert.equal(linkValid(d, d.links[0]), true);
    assert.equal(linkRule(d, d.links[0]), 'pulse');
  });
});

describe('multi rows (MULTI-01/02)', () => {
  test('capacity gates docking; growing a row unlocks a second source', () => {
    const d = doc([
      { id: 1, type: 'button', col: 1, row: 1 },  // pressed out at row 2
      { id: 2, type: 'wait', col: 1, row: 3 },    // done out at row 4
      { id: 3, type: 'action', col: 2, row: 1 },  // trigger in at row 2, capacity 1
    ], [[1, 'pressed', 3, 'trigger']]);
    // Full: wait.done (row 4) is too far anyway, move wait so done sits at row 3.
    d.nodes[1].row = 2;                            // done at row 3 → dRow 1 to trigger
    assert.equal(settleLinks(d, 2).added, 0);      // capacity 1 already used
    assert.equal(setPortRows(d, d.nodes[2], 'in', 'trigger', +1), true);
    assert.equal(portCapacity(d.nodes[2], 'in', 'trigger'), 2);
    const r = settleLinks(d, 2);                   // trigger row 3 now exists
    assert.equal(r.added, 1);
    assert.equal(portLoad(d, 3, 'in', 'trigger'), 2);
  });

  test('shrinking below the connected load is refused', () => {
    const d = doc([
      { id: 1, type: 'button', col: 1, row: 1 },
      { id: 3, type: 'action', col: 2, row: 1, rep: { 'in:trigger': 1 } },
    ], [[1, 'pressed', 3, 'trigger']]);
    assert.equal(setPortRows(d, d.nodes[1], 'in', 'trigger', -1), true);   // load 1, min 1 ok
    assert.equal(setPortRows(d, d.nodes[1], 'in', 'trigger', -1), false);  // below 1 row
  });

  test('a repeatable output fans out to several neighbors', () => {
    const d = doc([
      { id: 1, type: 'constant', col: 1, row: 1, rep: { 'out:value': 1 } },  // value rows 2+3
      { id: 2, type: 'log', col: 2, row: 1 },   // value in at row 2
      { id: 3, type: 'log', col: 2, row: 3 },   // value in at row 4 (±1 of value row 3)
    ]);
    const r = settleLinks(d, 1);
    assert.equal(r.added, 2);
    assert.equal(portLoad(d, 1, 'out', 'value'), 2);
    assert.deepEqual(validateDoc(d), []);
  });
});

describe('validateDoc (VAL-02)', () => {
  test('accepts a consistent document and flags overlap + undockable links', () => {
    const good = doc([
      { id: 1, type: 'button', col: 1, row: 1 },
      { id: 2, type: 'wait', col: 2, row: 1 },
    ], [[1, 'pressed', 2, 'start']]);
    assert.deepEqual(validateDoc(good), []);

    const bad = doc([
      { id: 1, type: 'button', col: 1, row: 1 },
      { id: 2, type: 'timer', col: 1, row: 2 },
    ], [[1, 'pressed', 2, 'start']]);
    const errors = validateDoc(bad);
    assert.ok(errors.some(e => e.includes('overlap')));
    assert.ok(errors.some(e => e.includes('not dockable')));
  });

  test('flags links beyond a port\'s row capacity', () => {
    const d = doc([
      { id: 1, type: 'button', col: 1, row: 1 },
      { id: 2, type: 'wait', col: 1, row: 3 },
      { id: 3, type: 'action', col: 2, row: 1 },
    ], [[1, 'pressed', 3, 'trigger'], [2, 'done', 3, 'trigger']]);
    d.nodes[1].row = 2;  // done at row 3, within reach of trigger row 2
    assert.ok(validateDoc(d).some(e => e.includes('exceeds its row capacity')));
  });

  test('every registry entry produces a positive height', () => {
    for (const t of Object.keys(NODE_TYPES)) assert.ok(nodeHeight(t) >= 2, t);
  });
});
