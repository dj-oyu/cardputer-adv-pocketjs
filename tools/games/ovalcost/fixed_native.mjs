// "What if pose() were native, in fixed point?" (docs/apps/derby-trig-cull.md
// section 9): the bend's pose() in C integer arithmetic, reproduced exactly
// in JS (BigInt where C would use int64_t), against Math.cos/sin, over every
// bend pose the panning frames make (trig_cull.mjs). Nothing here runs on the
// device; no native code is changed.
//
//   node tools/games/ovalcost/fixed_native.mjs [--ref 44fce25]
import { runSet } from './trig_cull.mjs';

const OB = 650 - 120 * Math.PI, DNR = 11;
// A quarter-wave sine table of n intervals in Q(q): int16 for q 15, int32 for
// q 30; const in C, so flash, no guest or DRAM bytes.
function table(n, q) { const t = []; for (let i = 0; i <= n + 1; ++i) t.push(Math.round(Math.sin(i * Math.PI / 2 / n) * 2 ** q)); return t; }
// sin, cos of a heading in Q(A) radians, in Q(q): the index on the quarter,
// linear interpolation in integers, folded by symmetry (sin(pi - x) = sin x,
// cos x = sin(pi/2 - x)).
function sc(a, T, n, q, A) {
  const QUART = BigInt(Math.round(Math.PI / 2 * 2 ** A));
  const at = x => {
    const y = x > QUART ? 2n * QUART - x : x, p = y * BigInt(n) * 65536n / QUART, i = Number(p >> 16n), f = p & 65535n;
    return BigInt(T[i]) + ((BigInt(T[i + 1] - T[i]) * f) >> 16n);
  };
  return [a <= QUART ? at(QUART - a) : -at(a - QUART), at(a)];
}
// pose() on the bend: x = 650 + S r - w S, z = -218 - (C + 1) r + w C, r = -109
// m; positions in Q16 m (int32: |x| < 32768 m).
function poseFix(g, w, T, n, q, A) {
  const a = BigInt(Math.round(Math.PI * 2 ** A) - Math.round((g - OB) / 120 * 2 ** A));
  const [C, S] = sc(a, T, n, q, A), ONE = 1n << BigInt(q), Q = BigInt(q);
  const mul = (v, k) => (v * BigInt(Math.round(k * 65536))) >> Q; // Q(q) x Q16 -> Q16
  const x = 650n * 65536n + mul(S, -109) - mul(S, w), z = -218n * 65536n - mul(C + ONE, -109) + mul(C, w);
  return [Number(x) / 65536, Number(z) / 65536];
}
function poseExact(g, w) {
  const a = Math.PI - (g - OB) / 120, C = Math.cos(a), S = Math.sin(a), r = -120 + DNR;
  return [650 + S * r - w * S, -218 - (C + 1) * r + w * C];
}

const L = []; for (let x = Math.ceil(OB); x < 650; x += 5) L.push(x);
const fr = [[-100, 0], [-14, 0], [-14, 8], [-3, 0], [-3, 8]].flatMap(([w2, cam]) => runSet(null, true, w2, L, cam).out);
console.log('| 1/4 周期の表 | 角度 | 要素 | flash B | 最大誤差 m | 最大誤差 px |');
console.log('| --- | --- | ---: | ---: | ---: | ---: |');
for (const [n, q, A] of [[64, 15, 16], [256, 30, 16], [64, 15, 28], [256, 15, 28], [64, 30, 28], [256, 30, 28], [1024, 30, 28]]) {
  const T = table(n, q);
  let m = 0, px = 0;
  for (const f of fr) {
    const pc = f.g.pc;
    for (const p of f.poses) {
      if (!(p[1] > OB && p[1] <= 650)) continue;
      for (const w of p[2] === 0 ? [11, 22.6, 40] : [p[2]]) {
        const E = poseExact(p[1], w), B = poseFix(p[1], w, T, n, q, A), d = Math.hypot(E[0] - B[0], E[1] - B[1]);
        const Z = ((E[0] - pc[0]) * pc[2] + (E[1] - pc[1]) * pc[3]) / pc[4];
        m = Math.max(m, d); if (Z > .02) px = Math.max(px, d / Z);
      }
    }
  }
  console.log(`| ${n} 区間、Q${q}（${q > 15 ? 'int32' : 'int16'}） | Q${A} rad | ${n + 2} | ${(n + 2) * (q > 15 ? 4 : 2)} | ${m.toExponential(1)} | ${px.toExponential(1)} |`);
}
