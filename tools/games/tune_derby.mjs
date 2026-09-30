// Monte Carlo over apps/derby/derby_watch.js's race model (host, Node).
// Loads the app with no `pocket`, so only globalThis.derby is defined, and
// reports what the tuning of the model and of the odds rests on: winners by
// running style, the favourite's win rate, finishing margins, lead changes in
// the last 300 m, race time; then the odds: a conditional-logit (Luce) fit of
// the winners on the paddock figures the app's odds() scores (the fitted
// weights are what odds() carries, rounded), and the calibration of the app's
// own odds by predicted-chance bin and by popularity rank: predicted vs
// observed win rate and the bettor's return per point staked (EV; the 20%
// take makes a fair book 0.8 everywhere), with its standard error.
//
//   node tools/games/tune_derby.mjs [races] [--check]
// --check exits 1 unless every bin and rank returns 0.8 within 0.06 plus 2.5
// standard errors (the longshots win a few hundred times in 20,000 races, so
// their EV carries +-0.05 of noise; the favourites' +-0.01).
// tools/games/run_derby.py runs it with 20,000 races. Races are seeded 1..N
// as the game once was; the model does not care where a seed comes from.
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

let src = readFileSync(new URL('../../apps/derby/derby_watch.js', import.meta.url), 'utf8');
// DERBY_SET="FORM=1,SPR=.2" tries other model constants without editing the app.
for (const kv of (process.env.DERBY_SET || '').split(',').filter(Boolean)) {
  const [k, v] = kv.split('=');
  const re = new RegExp('\\b' + k + ' = [-.0-9]+');
  if (!re.test(src)) throw Error('no constant ' + k);
  src = src.replace(re, k + ' = ' + v);
}
const ctx = {Math, console};
ctx.globalThis = ctx;
vm.createContext(ctx);
vm.runInContext(src, ctx);
const {field, race, step, order, odds, D, DT} = ctx.derby;

const args = process.argv.slice(2), check = args.includes('--check');
const N = +(args.find(a => !a.startsWith('--')) || 3000);
const styleWins = [0, 0, 0], styleCount = [0, 0, 0];
let favWins = 0, times = [], margins = [], changes = [], rows = [];
for (let n = 0; n < N; ++n) {
  const seed = (0x3e1b7 + Math.imul(n + 1, 0x9e3779b9)) >>> 0;
  const f = field(seed), od = odds(f.h);
  const s = race(f, true);
  let lead = -1, ch = 0;
  while (s.done < 8 && s.t < 200) {
    step(f, s, DT);
    const l = order(s)[0];
    if (Math.max(...s.x) > D - 300 && l !== lead && lead >= 0 && !s.done) ch++;
    lead = l;
  }
  const o = order(s), w = o[0];
  for (let i = 0; i < 8; ++i) styleCount[f.h[i].sty]++;
  styleWins[f.h[w].sty]++;
  if (od.indexOf(Math.min(...od)) === w) favWins++;
  times.push(s.tc[w]);
  margins.push((s.tc[o[1]] - s.tc[w]) * s.v[o[1]]);
  changes.push(ch);
  rows.push({h: f.h, od: Array.from(od), w});
}
const q = (a, p) => { const b = a.slice().sort((x, y) => x - y); return b[Math.floor(p * (b.length - 1))]; };
const frac = (a, f) => a.filter(f).length / a.length;
console.log(`races ${N}`);
console.log(`win share by style (FRONT/STALK/CLOSE): ${styleWins.map((w, i) => (100 * w / N).toFixed(1) + '%').join(' / ')}` +
  `  (field share ${styleCount.map(c => (100 * c / N / 8).toFixed(1) + '%').join(' / ')})`);
console.log(`favourite (shortest odds, first of equals) wins: ${(100 * favWins / N).toFixed(1)}%  (uniform 12.5%)`);
console.log(`winner time s: p10 ${q(times, .1).toFixed(2)} median ${q(times, .5).toFixed(2)} p90 ${q(times, .9).toFixed(2)}`);
console.log(`margin 1st-2nd m: median ${q(margins, .5).toFixed(2)}; under 0.12 m (NOSE) ${(100 * frac(margins, m => m < .12)).toFixed(1)}%,` +
  ` under 0.6 m (HEAD or closer) ${(100 * frac(margins, m => m < .6)).toFixed(1)}%, over 2.4 m ${(100 * frac(margins, m => m > 2.4)).toFixed(1)}%`);
console.log(`lead changes in the last 300 m: median ${q(changes, .5)}, p90 ${q(changes, .9)}, none ${(100 * frac(changes, c => c === 0)).toFixed(1)}%`);

// Conditional logit: P(i wins) = exp(b.x_i) / sum_j exp(b.x_j), fitted by
// Newton's method (the log likelihood is concave). x is what odds() scores.
const FEAT = ['top', 'st', 'sty>1', 'acc', 're'];
const X = h => h.map(k => [k.top, k.st, +(k.sty > 1), k.acc, k.re]);
const K = FEAT.length;
function soft(x, b) {
  const u = x.map(v => v.reduce((a, c, k) => a + c * b[k], 0)), m = Math.max(...u), e = u.map(v => Math.exp(v - m));
  const z = e.reduce((a, c) => a + c);
  return e.map(v => v / z);
}
let b = Array(K).fill(0), L = 0;
for (let it = 0; it < 40; ++it) {
  const g = Array(K).fill(0), H = [...Array(K)].map(() => Array(K).fill(0));
  L = 0;
  for (const {h, w} of rows) {
    const x = X(h), p = soft(x, b), mu = Array(K).fill(0);
    L += Math.log(p[w]);
    for (let i = 0; i < 8; ++i) for (let k = 0; k < K; ++k) mu[k] += p[i] * x[i][k];
    for (let k = 0; k < K; ++k) g[k] += x[w][k] - mu[k];
    for (let i = 0; i < 8; ++i) for (let k = 0; k < K; ++k) for (let l = 0; l < K; ++l) H[k][l] += p[i] * (x[i][k] - mu[k]) * (x[i][l] - mu[l]);
  }
  // Solve H s = g (H is the negated Hessian, positive definite).
  const A = H.map((r, k) => r.concat([g[k]]));
  for (let c = 0; c < K; ++c) {
    for (let r = 0; r < K; ++r) if (r !== c) { const f = A[r][c] / A[c][c]; for (let j = c; j <= K; ++j) A[r][j] -= f * A[c][j]; }
  }
  const s = A.map((r, k) => r[K] / r[k]);
  b = b.map((v, k) => v + s[k]);
  if (s.every(v => Math.abs(v) < 1e-9)) break;
}
console.log(`odds fit (conditional logit, ${N} races): ${FEAT.map((f, k) => f + ' ' + b[k].toFixed(3)).join(', ')}; ` +
  `mean log-likelihood ${(L / N).toFixed(4)} (uniform ${Math.log(1 / 8).toFixed(4)})`);

// Calibration of the app's odds: its chance is 0.8/odds (before rounding and
// the 1.1..99.9 clamp); the bettor's return per point on a runner is odds if
// it wins, 0 if not.
const EDGES = [0, .05, .1, .2, .3, .5, 1];
const group = () => ({n: 0, p: 0, w: 0, r: 0, r2: 0});
const bins = EDGES.slice(1).map(group), ranks = [...Array(8)].map(group);
let evAll = 0;
for (const {od, w} of rows) {
  const rk = [0, 1, 2, 3, 4, 5, 6, 7].sort((a, c) => od[a] - od[c] || a - c);
  for (let i = 0; i < 8; ++i) {
    const p = .8 / od[i], r = i === w ? od[i] : 0;
    let k = 0;
    while (k < EDGES.length - 2 && p >= EDGES[k + 1]) ++k;
    for (const g of [bins[k], ranks[rk.indexOf(i)]]) { g.n++; g.p += p; g.w += i === w; g.r += r; g.r2 += r * r; }
    evAll += r;
  }
}
console.log(`odds calibration (return per point staked; 0.8 is a fair book after the 20% take; overall ${(evAll / N / 8).toFixed(3)}):`);
let fails = 0, worst = 0;
const line = (g, name) => {
  if (!g.n) return;
  const ev = g.r / g.n, se = Math.sqrt((g.r2 / g.n - ev * ev) / g.n), tol = .06 + 2.5 * se, bad = Math.abs(ev - .8) > tol;
  fails += bad;
  worst = Math.max(worst, Math.abs(ev - .8));
  console.log(`  ${name.padEnd(11)} n ${String(g.n).padStart(6)}  predicted ${(100 * g.p / g.n).toFixed(1).padStart(5)}%  ` +
    `observed ${(100 * g.w / g.n).toFixed(1).padStart(5)}%  EV ${ev.toFixed(3)} +- ${se.toFixed(3)}` +
    (check ? `  (0.8 +- ${tol.toFixed(3)}${bad ? ' OUT' : ''})` : ''));
};
bins.forEach((g, i) => line(g, `p ${EDGES[i]}-${EDGES[i + 1]}`));
ranks.forEach((g, i) => line(g, `favourite ${i + 1}`));
console.log(`race 1 odds ${rows[0].od.join(',')}`);
if (check) {
  console.log(`odds check: worst |EV - 0.8| ${worst.toFixed(3)}, ${fails} group(s) outside 0.06 + 2.5 SE`);
  if (fails) { console.log('ODDS_CHECK FAIL'); process.exit(1); }
  console.log('ODDS_CHECK PASS');
}
