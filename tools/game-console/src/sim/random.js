// Small seeded PRNG (mulberry32) so simulator runs and tests repeat.
export function makeRandom(seed = 1) {
  let a = seed >>> 0;
  const next = () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
  const rnd = () => next();
  rnd.int = (lo, hi) => lo + Math.floor(next() * (hi - lo + 1)); // inclusive
  rnd.chance = (p) => next() < p;
  rnd.pick = (arr) => arr[Math.floor(next() * arr.length)];
  rnd.normal = () => {
    const u = Math.max(next(), 1e-9);
    const v = next();
    return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v);
  };
  return rnd;
}
