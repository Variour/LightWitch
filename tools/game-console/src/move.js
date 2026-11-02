// The board of a `move` round: cells, start cells and how a buzzer key
// turns into a step on the board. The console owns the game state; the
// buzzers only report their real keys.

export const HOLDS = ['quer', 'hochkant'];
export const GOALS = ['right'];
export const MOVE_DEFAULTS = { hold: 'quer', goal: 'right', cols: 22, rows: 10 };

const STEP = {
  up: { dc: 0, dr: -1 },
  right: { dc: 1, dr: 0 },
  down: { dc: 0, dr: 1 },
  left: { dc: -1, dr: 0 },
};

// Board direction of a buzzer key. "quer": the buzzer is turned 90° clockwise,
// buzzer on the right, so its up key points right. "hochkant": as printed.
const BOARD_DIR = {
  quer: { left: 'up', up: 'right', right: 'down', down: 'left' },
  hochkant: { up: 'up', right: 'right', down: 'down', left: 'left' },
};

export function boardDirection(hold, key) {
  const map = BOARD_DIR[hold];
  return map ? map[key] ?? null : null;
}

// The buzzer key that moves a figure in this board direction.
export function deviceKey(hold, dir) {
  const map = BOARD_DIR[hold];
  if (!map) return null;
  for (const [key, d] of Object.entries(map)) if (d === dir) return key;
  return null;
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
