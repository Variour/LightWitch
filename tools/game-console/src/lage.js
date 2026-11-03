// Orientation ("Lage") of a buzzer and the round field `hold`. Provisional
// until the firmware reads its orientation sensor: the buzzer then remaps its
// buttons itself and reports logical inputs together with its lage.

export const LAGEN = ['hochkant', 'quer', 'quer-links', 'liegend'];
// frei: the buzzer follows its lage. hochkant, quer, liegend: required lage,
// the buzzer is locked in any other. fest: the lage at the open stays.
export const HOLDS = ['frei', 'hochkant', 'quer', 'liegend', 'fest'];
export const LOGICAL_KEYS = ['up', 'right', 'down', 'left', 'triggerLeft', 'triggerRight', 'buzzer', 'enter'];

// Physical button for each logical input, per lage. Buzzer and enter stay.
const PHYSICAL = {
  hochkant: { up: 'up', right: 'right', down: 'down', left: 'left' },
  quer: { up: 'left', right: 'up', down: 'right', left: 'down', triggerLeft: 'l2', triggerRight: 'l1' },
  'quer-links': { up: 'right', right: 'down', down: 'left', left: 'up', triggerLeft: 'r1', triggerRight: 'r2' },
  liegend: { up: 'up', right: 'right', down: 'down', left: 'left' },
};
const FIXED = { buzzer: 'buzz', enter: 'enter' };

export const isLage = (v) => LAGEN.includes(v);

// The lage a round requires, or null (frei, fest, idle).
export function requiredLage(hold) {
  return hold === 'hochkant' || hold === 'quer' || hold === 'liegend' ? hold : null;
}

// Whether a buzzer in this lage meets the required one: "quer-links" (across,
// buzzer on the left) counts as "quer".
export function fitsLage(lage, need) {
  return lage === need || (need === 'quer' && lage === 'quer-links');
}

// The physical button that gives this logical input in this lage, or null.
export function toPhysical(lage, logical) {
  if (FIXED[logical]) return FIXED[logical];
  const map = PHYSICAL[lage];
  return (map && map[logical]) ?? null;
}

// The logical input of a physical button in this lage, or null.
export function toLogical(lage, physical) {
  for (const [logical, button] of Object.entries(FIXED)) if (button === physical) return logical;
  const map = PHYSICAL[lage];
  if (!map) return null;
  for (const [logical, button] of Object.entries(map)) if (button === physical) return logical;
  return null;
}
