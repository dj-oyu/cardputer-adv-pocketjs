// How far Shiki's integer CUBIC (docs/kasane/shiki-spec.md sec.4.6, v1.2)
// lands from the exact curve, beside float32 and the alternatives weighed
// for it. Host, Node only. Results: docs/kasane/shiki.md sec.5.10.
//
//   node tools/kasane_ir/cubic_precision.mjs [--curves N] [--seed S]
//
// Curves: control points uniform in -480..720 px (Q16, whole raw units),
// segment counts uniform in 1..255, from a fixed LCG. "Exact" is the
// Bernstein form in double at t = k/n; a coordinate "differs" when its
// pixel is not that exact value rounded half up (sec.4.5).
const argv = process.argv.slice(2);
const opt = (name, d) => { const i = argv.indexOf(name); return i < 0 ? d : Number(argv[i + 1]); };
const CURVES = opt('--curves', 200000);
let st = opt('--seed', 12345);
const rnd = () => (st = (st * 1103515245 + 12345) % 2147483648) / 2147483648;
const B = BigInt;
const fdiv = (a, d) => { let q = a / d; if (a % d !== 0n && (a < 0n) !== (d < 0n)) q -= 1n; return q; };
const pix = raw => Number(fdiv(raw + 32768n, 65536n));
const F = Math.fround;

// Each takes raw Q16 control points (8 BigInt), n, k; returns [x, y] in px.
const MODES = {
  // v1.2: weights by MUL s=16, each term by MUL, summed by ADD.
  'v1.2': (P, n, k) => {
    const t = B(Math.floor(k * 65536 / n)), u = 65536n - t, uu = u * u >> 16n, tt = t * t >> 16n;
    const w = [uu * u >> 16n, 3n * (uu * t >> 16n), 3n * (u * tt >> 16n), tt * t >> 16n];
    return [0, 1].map(o => pix(w.reduce((s, wi, i) => s + fdiv(wi * P[2 * i + o], 65536n), 0n)));
  },
  'one floor': (P, n, k) => {
    const t = B(Math.floor(k * 65536 / n)), u = 65536n - t, uu = u * u >> 16n, tt = t * t >> 16n;
    const w = [uu * u >> 16n, 3n * (uu * t >> 16n), 3n * (u * tt >> 16n), tt * t >> 16n];
    return [0, 1].map(o => pix(fdiv(w.reduce((s, wi, i) => s + wi * P[2 * i + o], 0n), 65536n)));
  },
  'Q30 weights': (P, n, k) => {
    const t = B(Math.floor(k * 65536 / n)), u = 65536n - t;
    const w = [u * u * u >> 18n, 3n * u * u * t >> 18n, 3n * u * t * t >> 18n, t * t * t >> 18n];
    return [0, 1].map(o => pix(fdiv(w.reduce((s, wi, i) => s + wi * P[2 * i + o], 0n), 1n << 30n)));
  },
  'sum 2^16': (P, n, k) => {
    const t = B(Math.floor(k * 65536 / n)), u = 65536n - t, uu = u * u >> 16n, tt = t * t >> 16n;
    const w0 = uu * u >> 16n, w1 = 3n * (uu * t >> 16n), w3 = tt * t >> 16n, w = [w0, w1, 65536n - w0 - w1 - w3, w3];
    return [0, 1].map(o => pix(fdiv(w.reduce((s, wi, i) => s + wi * P[2 * i + o], 0n), 65536n)));
  },
  // The old API's float32 evaluation, rounded half up.
  float32: (P, n, k) => {
    const t = F(k / n), u = F(1 - t);
    return [0, 1].map(o => {
      const p = i => F(Number(P[2 * i + o]) / 65536);
      const v = F(F(F(F(F(F(u * u) * u) * p(0)) + F(F(F(F(3 * u) * u) * t) * p(1))) + F(F(F(F(3 * u) * t) * t) * p(2))) + F(F(F(t * t) * t) * p(3)));
      return Math.floor(v + .5);
    });
  },
};
const rows = Object.fromEntries(Object.keys(MODES).map(m => [m, {worst: 0, diff: 0}]));
let coords = 0;
for (let c = 0; c < CURVES; c++) {
  const P = Array.from({length: 8}, () => B(Math.round((rnd() * 1200 - 480) * 65536)));
  const n = 1 + Math.floor(rnd() * 255);
  for (let k = 0; k <= n; k++) {
    const tt = k / n, uu = 1 - tt, wt = [uu * uu * uu, 3 * uu * uu * tt, 3 * uu * tt * tt, tt * tt * tt];
    const exact = [0, 1].map(o => wt.reduce((s, wi, i) => s + wi * Number(P[2 * i + o]) / 65536, 0));
    coords += 2;
    for (const [m, f] of Object.entries(MODES)) {
      const got = f(P, n, k), r = rows[m];
      for (const o of [0, 1]) {
        r.worst = Math.max(r.worst, Math.abs(got[o] - exact[o]));
        if (got[o] !== Math.floor(exact[o] + .5)) r.diff++;
      }
    }
  }
}
console.log(`curves ${CURVES}, coordinates ${coords}`);
for (const [m, r] of Object.entries(rows))
  console.log(`${m.padEnd(12)} max ${r.worst.toFixed(3)} px from exact, ${r.diff} differ (${(100 * r.diff / coords).toFixed(3)}%)`);
