// MEGADEMO, host tests only: procMegademo, the form the host harnesses
// read, evaluated after the app's chunks (tools/kasane_contract/
// megademo_chunks.py appends this file). It used to sit in the act2 chunk,
// where the device kept ~1.5 KB of it resident for nothing
// (docs/apps/megademo-story.md, 2026-10-29).
'use strict';
// [rows, points or null] of a plan entry: the template decoded
// (megademo_plans.js), as the firmware's table holds it.
const plan = e => [prog(T[e[0]], e.slice(1)), e[0] === 'sh' ? points(e[1]) : null];
// The host tests' form: specs() a thunk per plan; frame() {b: backdrop, s,
// d: [[plan, [inputs]]...]} (plan -1: a pocket.shapes segment).
globalThis.procMegademo = {names: NAMES, len: LEN, tiers: KN.length, specs: (s, tier) => plist(s, tier).map(e => () => plan(e)),
  frame: function (s, tier, t) {
  const r = {b: 0, s: 0, d: []}, e = E, g = BG;
  E = (k, ...a) => { r.d.push([k, a]); };
  BG = b => { r.b = b; };
  play(s, tier, t);
  E = e; BG = g;
  return r;
}};
