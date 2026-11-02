// The board of a `move` round: cells, start cells and steps. The console owns
// the game state; the buzzers only report their keys.

import { toLogical } from './lage.js';

export const GOALS = ['right'];
export const MOVE_DEFAULTS = { goal: 'right', cols: 22, rows: 10 };
export const DIRECTIONS = ['up', 'right', 'down', 'left'];

const STEP = {
  up: { dc: 0, dr: -1 },
  right: { dc: 1, dr: 0 },
  down: { dc: 0, dr: 1 },
  left: { dc: -1, dr: 0 },
};

// Board direction of a key line. A line with a lage carries a logical key.
// Without one the key is the physical button: until the buzzers remap
// themselves, the console turns them for "quer" (left is up, up is right,
// right is down, down is left) and takes them as printed otherwise.
export function boardDirection(hold, key, lage) {
  const logical = lage ? key : hold === 'quer' ? toLogical('quer', key) : key;
  return DIRECTIONS.includes(logical) ? logical : null;
}

export function step(cell, dir) {
  const s = STEP[dir];
  return { col: cell.col + s.dc, row: cell.row + s.dr };
}

// Cells are 0-based (col, row), col 0 left, row 0 top. Numbers fill the
// board column by column from the left: with 10 rows 1-10 in col 0, 11-20 in col 1.
export function startCell(n, rows) {
  return { col: Math.floor((n - 1) / rows), row: (n - 1) % rows };
}

export function inside(cell, cols, rows) {
  return cell.col >= 0 && cell.col < cols && cell.row >= 0 && cell.row < rows;
}

export function atGoal(cell, goal, cols) {
  return goal === 'right' && cell.col === cols - 1;
}
