// How far LINE_PATTERN's cut coordinate (docs/kasane/shiki-spec.md sec.4.7,
// v1.2: weights normalised to 13 bits, the ratio in Q14, so every division is
// 32-bit) lands from the exact perspective-correct value, on DERBY's captured
// crowd draws, beside two other splits of the same precision budget. Host,
// Node only. Results: docs/kasane/shiki.md sec.5.10.
//
//   node tools/kasane_ir/pattern_cut_precision.mjs [CASES.json]
//
// CASES.json defaults to .cache/kasane_ir/q16_pan_cases.json (q16_error.py's
// capture of the panning draws). Each crowd draw's rows are rebuilt as the
// plan builds them (u moves by the shift argument a row); "exact" is
// R(u0 w0 (c-j) + u1 w1 j, w0 (c-j) + w1 j) in BigInt.
import fs from 'node:fs';
const B = BigInt, fdiv = (a, d) => { if (d < 0n) { a = -a; d = -d; } let q = a / d; if (a % d !== 0n && a < 0n) q -= 1n; return q; };
const R64 = (p, q) => fdiv(2n * p + q, 2n * q);
const Rn = (p, q) => { if (q < 0) { p = -p; q = -q; } return Math.floor((2 * p + q) / (2 * q)); }; // exact for |p| < 2^51
const q16 = v => Math.round(v * 65536);
const cases = JSON.parse(fs.readFileSync(process.argv[2] ?? '.cache/kasane_ir/q16_pan_cases.json', 'utf8')).filter(c => c.plan === 'crowd');
const chords = (w0, w1) => { if (!((w0 > 0 && w1 > 0) || (w0 < 0 && w1 < 0))) return 1; const W = Math.max(Math.abs(w0), Math.abs(w1)), V = Math.min(Math.abs(w0), Math.abs(w1));
  return 50 * W < 51 * V ? 1 : 5 * W < 6 * V ? 2 : 5 * W < 8 * V ? 4 : 8; };
const variants = {};
for (const [wb, lq] of [[13, 14], [15, 12], [11, 16]]) variants[`w${wb} λQ${lq}`] = (u0, u1, w0, w1, c, j) => {
  let a = Math.abs(w0), b = Math.abs(w1); const e = Math.max(0, Math.max(a, b).toString(2).length - wb); a = a >> e; b = b >> e; // floor of positive
  const lam = Rn(b * j * 2 ** lq, a * (c - j) + b * j);
  return u0 + Number(R64(B(u1 - u0) * B(lam), B(2 ** lq)));
};
const err = Object.fromEntries(Object.keys(variants).map(k => [k, {max: 0, d88: 0, n: 0}]));
let lines = 0, cuts = 0;
for (const c of cases) { const shift = c.args[6];
  for (const inp of c.inputs) { const [, , , , dy0, dy1, u0, u1] = inp; const w0 = q16(dy0), w1 = q16(dy1);
    for (let row = 0; row < c.args[0]; row++) { const ua = q16(u0 + row * shift), ub = q16(u1 + row * shift); lines++;
      const n = chords(w0, w1);
      for (let j = 1; j < n; j++) { cuts++;
        const exact = Number(R64(B(ua) * B(w0) * B(n - j) + B(ub) * B(w1) * B(j), B(w0) * B(n - j) + B(w1) * B(j)));
        for (const [k, f] of Object.entries(variants)) { const v = f(ua, ub, w0, w1, n, j), r = err[k];
          r.max = Math.max(r.max, Math.abs(v - exact) / 65536); r.n++; if (Rn(v, 256) !== Rn(exact, 256)) r.d88++; } } } } }
console.log({lines, cuts});
for (const [k, r] of Object.entries(err)) console.log(k, 'max err (cells)', r.max.toFixed(5), '8.8 phase differs', r.d88, 'of', r.n);
