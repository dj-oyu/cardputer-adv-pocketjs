// MEGADEMO chunk 1: shared maths and the load tiers (the plan templates are
// megademo_plans.js, built into the firmware). Every distinct name (global or local) is a resident
// atom (~60 B): the chunks reuse short names and write out numbers used
// once (docs/apps/megademo-story.md, 2026-10-29).
'use strict';
const M = Math, rnd = M.round, PI = M.PI;
const ramp = (t, a, b) => t <= a ? 0 : t >= b ? 1 : (t - a) / (b - a), smooth = u => u * u * (3 - 2 * u);
// Apple II hi-res palette (violet 0xfa3f, green 0x17a7, blue 0x167f inline).
const WH = 0xffff, OR = 0xfb47;
// Scenes draw through E(plan, inputs...) after BG(backdrop): the loop's
// draw (numbers, no array per draw), or procMegademo.frame()'s list.
let E = null, BG = null;
// [LIGHT, MID, HEAVY]. c: TWIST corridor (n frames, r ratio, m rungs per
// frame, s arch segments, kf/ka first frame/arch, D twist); z: ZENITH;
// k: CORE (r rays a draw, d ray draws, n noise lines). Derivation:
// docs/kasane/megademo-limit-scenes.md, megademo-device-limits.md.
// TWIST and ZENITH hold the display rate at HEAVY's load, so MID shares it.
const CH = {n: 26, r: .8, m: 3, s: 8, kf: 2, ka: 3, D: 6},
  ZH = {st: 60, gl: 14, ln: 15, cr: 3, cc: 8, sa: 4, ts: 16, rr: 4};
const KN = [
  {c: {n: 12, r: .7, m: 2, s: 4, kf: 2, ka: 3, D: 4}, z: {st: 24, gl: 8, ln: 7, cr: 2, cc: 4, sa: 2, ts: 6, rr: 2},
   k: {r: 20, d: 1, n: 12}},
  {c: CH, z: ZH, k: {r: 40, d: 1, n: 24}},
  {c: CH, z: ZH, k: {r: 40, d: 2, n: 40}}
];
// hx: a hex digit of a string (the tables in megademo_act2.js). The plan
// templates are megademo_plans.js, compiled into the firmware.
const hx = (s, i) => { const c = s.charCodeAt(i); return c > 57 ? c - 87 : c - 48; };
