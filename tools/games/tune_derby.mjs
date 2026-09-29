// Monte Carlo over apps/derby/derby_watch.js's race model (host, Node).
// Loads the app with no `pocket`, so only globalThis.derby is defined, and
// reports what the tuning of the model and of the odds temperature TAU rests
// on: winners by running style, favourite win rate, finishing margins, lead
// changes in the last 300 m, race time, and the TAU that maximises the
// likelihood of the observed winners under the app's softmax.
//
//   node tools/games/tune_derby.mjs [races]
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
const {field, race, step, order, odds, D, DT, TAU} = ctx.derby;

const N = +(process.argv[2] || 3000);
const styleWins = [0, 0, 0], styleCount = [0, 0, 0];
let favWins = 0, times = [], margins = [], changes = [], rows = [];
for (let n = 0; n < N; ++n) {
  const seed = (0x3e1b7 + Math.imul(n + 1, 0x9e3779b9)) >>> 0;
  const f = field(seed);
  const solo = race(f, false);
  while (solo.done < 8) step(f, solo, .25);
  const T = solo.tc.slice();
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
  const fav = T.indexOf(Math.min(...T));
  if (fav === w) favWins++;
  times.push(s.tc[w]);
  margins.push((s.tc[o[1]] - s.tc[w]) * s.v[o[1]]);
  changes.push(ch);
  rows.push({T, w});
}
const q = (a, p) => { const b = a.slice().sort((x, y) => x - y); return b[Math.floor(p * (b.length - 1))]; };
const frac = (a, f) => a.filter(f).length / a.length;
// Log likelihood of the winners under softmax(-T/tau).
function ll(tau) {
  let s = 0;
  for (const {T, w} of rows) {
    const m = Math.min(...T);
    let z = 0;
    for (const t of T) z += Math.exp((m - t) / tau);
    s += (m - T[w]) / tau - Math.log(z);
  }
  return s / rows.length;
}
let best = 0, bl = -1e9;
for (let tau = .05; tau <= 3; tau += .01) { const l = ll(tau); if (l > bl) { bl = l; best = tau; } }
console.log(`races ${N}`);
console.log(`win share by style (FRONT/STALK/CLOSE): ${styleWins.map((w, i) => (100 * w / N).toFixed(1) + '%').join(' / ')}` +
  `  (field share ${styleCount.map(c => (100 * c / N / 8).toFixed(1) + '%').join(' / ')})`);
console.log(`favourite (fastest noise-free run) wins: ${(100 * favWins / N).toFixed(1)}%  (uniform 12.5%)`);
console.log(`winner time s: p10 ${q(times, .1).toFixed(2)} median ${q(times, .5).toFixed(2)} p90 ${q(times, .9).toFixed(2)}`);
console.log(`margin 1st-2nd m: median ${q(margins, .5).toFixed(2)}; under 0.12 m (NOSE) ${(100 * frac(margins, m => m < .12)).toFixed(1)}%,` +
  ` under 0.6 m (HEAD or closer) ${(100 * frac(margins, m => m < .6)).toFixed(1)}%, over 2.4 m ${(100 * frac(margins, m => m > 2.4)).toFixed(1)}%`);
console.log(`lead changes in the last 300 m: median ${q(changes, .5)}, p90 ${q(changes, .9)}, none ${(100 * frac(changes, c => c === 0)).toFixed(1)}%`);
console.log(`TAU: app ${TAU} (mean log-likelihood ${ll(TAU).toFixed(4)}), best ${best.toFixed(2)} (${bl.toFixed(4)}), uniform ${Math.log(1 / 8).toFixed(4)}`);
// Calibration: predicted win chance of each runner (app TAU) in bins vs observed.
const bins = [0, .05, .1, .2, .3, .5, 1].map(() => [0, 0]);
const edges = [0, .05, .1, .2, .3, .5, 1];
for (const {T, w} of rows) {
  const m = Math.min(...T);
  const p = T.map(t => Math.exp((m - t) / TAU));
  const z = p.reduce((a, b) => a + b, 0);
  p.forEach((x, i) => {
    const pr = x / z;
    let b = 0;
    while (b < edges.length - 2 && pr >= edges[b + 1]) ++b;
    bins[b][0] += pr; bins[b][1] += (i === w) ? 1 : 0; bins[b].n = (bins[b].n || 0) + 1;
  });
}
console.log('calibration (predicted vs observed win rate):');
for (let b = 0; b < edges.length - 1; ++b)
  if (bins[b].n) console.log(`  p ${edges[b]}-${edges[b + 1]}: n ${bins[b].n}, predicted ${(100 * bins[b][0] / bins[b].n).toFixed(1)}%, observed ${(100 * bins[b][1] / bins[b].n).toFixed(1)}%`);
const o = odds(rows[0].T);
console.log(`race 1 odds ${o.join(',')}`);
