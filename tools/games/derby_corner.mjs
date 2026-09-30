// Oval-course trial for DERBY WATCH (host, Node): a speed multiplier on the
// one bend of an oval, on top of apps/derby/derby_watch.js's race model. The
// app is not edited: its source is loaded twice, once as shipped and once
// with one line patched so step() multiplies each runner's progress by
// s.cm(i, x) when a race carries it. A race without s.cm is the shipped race.
//
// Oval: 1,600 m inner rail (423 m straights, 120 m radius half-circle bends),
// 1,000 m race started on the back straight: race distance 0-273 back
// straight, 273-650 the bend, 650-1000 the home straight. The bend is not
// measured lane by lane; it is a multiplier (docs/apps/derby-corner-model.md):
//   lane i (0 = rail):  1 + a * (3.5 - i) / 3.5
//   per runner:         * (1 + u_i),  u_i uniform in [-s, s]
// u_i comes from its own stream, rng(seed + 0x2545f491), so the race's own
// stream (seed + 0x5bd1e995) draws exactly what it draws on the straight.
//
//   node tools/games/derby_corner.mjs verify [seeds]     straight == shipped, bit for bit
//   node tools/games/derby_corner.mjs sweep [races] [a,s;a,s...] [--mode=x|g]
//   node tools/games/derby_corner.mjs fit [races] a s [--mode=x|g]   refit every weight (reference only)
//   node tools/games/derby_corner.mjs public [races] a s [--json]  odds from public information only
// a and s are fractions (0.005 = 0.5%). --mode=g multiplies the target speed
// instead of the progress (the stamina then pays for the rail).
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import {Worker, isMainThread, parentPort, workerData} from 'node:worker_threads';
import os from 'node:os';

const SELF = new URL(import.meta.url);
const SRC = readFileSync(new URL('../../apps/derby/derby_watch.js', SELF), 'utf8');
const B0 = 273, B1 = 650, CSEED = 0x2545f491;

function load(src) {
  const ctx = {Math, console};
  ctx.globalThis = ctx;
  vm.createContext(ctx);
  vm.runInContext(src, ctx);
  return ctx.derby;
}
function patched(mode) {
  const X = 'const y = s.x[i] = x + (s.v[i] = v) * dt;', G = 'const a = g - v, up = h.acc * dt;';
  if (!SRC.includes(X) || !SRC.includes(G)) throw Error('derby_watch.js step() changed: update the patch');
  return load(mode === 'g'
    ? SRC.replace(G, 'if (s.cm) g *= s.cm(i, x);\n    ' + G)
    : SRC.replace(X, 'const y = s.x[i] = x + (s.v[i] = v) * dt * (s.cm ? s.cm(i, x) : 1);'));
}
const seedOf = n => (0x3e1b7 + Math.imul(n + 1, 0x9e3779b9)) >>> 0;

// One oval race; returns the rows the main thread tallies.
function run(d, n, a, s) {
  const {field, race, step, order, odds, D, DT} = d;
  const f = field(seedOf(n)), od = odds(f.h), st = race(f);
  if (a || s) {
    const m = [];
    for (let i = 0; i < 8; ++i) m[i] = (1 + a * (3.5 - i) / 3.5) * (1 + (2 * CR(f.seed + CSEED, i) - 1) * s);
    st.cm = (i, x) => x >= B0 && x < B1 ? m[i] : 1;
  }
  let lead = -1, lcBend = 0, lcLast = 0, swBend = 0, swAll = 0, prev = null;
  while (st.done < 8 && st.t < 200) {
    step(f, st, DT);
    const o = order(st), l = o[0], xl = st.x[l];
    if (lead >= 0 && l !== lead && !st.done) {
      if (xl >= B0 && xl < B1) lcBend++;
      if (xl > D - 300) lcLast++;
    }
    lead = l;
    // Rank swaps: pairs whose order by distance flipped this step, neither
    // finished. Counted for the bend when either runner is in it.
    if (prev) for (let i = 0; i < 8; ++i) for (let j = i + 1; j < 8; ++j) {
      if (st.x[i] >= D || st.x[j] >= D) continue;
      if ((prev[i] - prev[j]) * (st.x[i] - st.x[j]) < 0) {
        swAll++;
        if ((st.x[i] >= B0 && st.x[i] < B1) || (st.x[j] >= B0 && st.x[j] < B1)) swBend++;
      }
    }
    prev = st.x.slice();
  }
  const o = order(st), w = o[0];
  return {h: f.h.map(k => [k.top, k.st, +(k.sty > 1), k.acc, k.re]), od: Array.from(od), w, sty: f.h[w].sty,
    t: st.tc[w], mg: (st.tc[o[1]] - st.tc[w]) * st.v[o[1]], lcBend, lcLast, swBend, swAll};
}
// The corner stream: the app's own rng() (mulberry32) on seed + CSEED, i-th draw.
function CR(seed, i) {
  let s = seed | 0, v = 0;
  for (let k = 0; k <= i; ++k) {
    s = s + 0x6d2b79f5 | 0;
    let t = Math.imul(s ^ s >>> 15, 1 | s);
    t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t;
    v = ((t ^ t >>> 14) >>> 0) / 4294967296;
  }
  return v;
}

if (!isMainThread) {
  const {mode, a, s, n0, n1} = workerData, d = patched(mode), out = [];
  for (let n = n0; n < n1; ++n) out.push(run(d, n, a, s));
  parentPort.postMessage(out);
} else main();

function runPar(N, a, s, mode) {
  const P = os.cpus().length, per = Math.ceil(N / P), jobs = [];
  for (let p = 0; p < P; ++p) {
    const n0 = p * per, n1 = Math.min(N, n0 + per);
    if (n0 >= n1) break;
    jobs.push(new Promise((res, rej) => {
      const w = new Worker(SELF, {workerData: {mode, a, s, n0, n1}});
      w.on('message', res); w.on('error', rej);
    }));
  }
  return Promise.all(jobs).then(r => r.flat());
}

// ---- Luce fit (conditional logit by Newton), as tune_derby.mjs, plus lane.
const FEAT = ['top', 'st', 'sty>1', 'acc', 're', 'lane'];
const X = r => r.h.map((v, i) => v.concat([(3.5 - i) / 3.5]));
// Xf gives the features per runner; off, if given, a fixed score added to
// each (its weight is not fitted).
function fit(rows, K, Xf = X, off = null) {
  let b = Array(K).fill(0), L = 0;
  for (let it = 0; it < 40; ++it) {
    const g = Array(K).fill(0), H = [...Array(K)].map(() => Array(K).fill(0));
    L = 0;
    for (const r of rows) {
      const x = Xf(r).map(v => v.slice(0, K)), o = off ? off(r) : null, u = x.map((v, i) => v.reduce((q, c, k) => q + c * b[k], o ? o[i] : 0)), m = Math.max(...u);
      const e = u.map(v => Math.exp(v - m)), z = e.reduce((q, c) => q + c), p = e.map(v => v / z), mu = Array(K).fill(0);
      L += Math.log(p[r.w]);
      for (let i = 0; i < 8; ++i) for (let k = 0; k < K; ++k) mu[k] += p[i] * x[i][k];
      for (let k = 0; k < K; ++k) g[k] += x[r.w][k] - mu[k];
      for (let i = 0; i < 8; ++i) for (let k = 0; k < K; ++k) for (let l = 0; l < K; ++l) H[k][l] += p[i] * (x[i][k] - mu[k]) * (x[i][l] - mu[l]);
    }
    const A = H.map((r, k) => r.concat([g[k]]));
    for (let c = 0; c < K; ++c) for (let r = 0; r < K; ++r) if (r !== c) { const f = A[r][c] / A[c][c]; for (let j = c; j <= K; ++j) A[r][j] -= f * A[c][j]; }
    const st = A.map((r, k) => r[K] / r[k]);
    b = b.map((v, k) => v + st[k]);
    if (st.every(v => Math.abs(v) < 1e-9)) break;
  }
  return {b, L: L / rows.length};
}
// The app's odds formula with weights w (lane weight last), tail c, cap 999.
function oddsW(r, w, c) {
  const p = [];
  let z = 0;
  const x = X(r);
  for (let i = 0; i < 8; ++i) {
    const k = x[i];
    z += p[i] = Math.exp(w[0] * k[0] + w[1] * k[1] + w[4] * k[4] + w[3] * (k[3] + k[2]) + (w[5] || 0) * k[5]);
  }
  for (let i = 0; i < 8; ++i) { const q = z / p[i]; p[i] = Math.max(1.1, Math.min(999, Math.round(8 * q * (1 + c * q)) / 10)); }
  return p;
}
// Return per point staked by popularity rank and predicted-chance bin.
const EDGES = [0, .05, .1, .2, .3, .5, 1];
function calib(rows, od) {
  const grp = () => ({n: 0, p: 0, w: 0, r: 0, r2: 0});
  const bins = EDGES.slice(1).map(grp), ranks = [...Array(8)].map(grp);
  let ev = 0;
  rows.forEach((row, n) => {
    const o = od(row, n), rk = [0, 1, 2, 3, 4, 5, 6, 7].sort((a, c) => o[a] - o[c] || a - c);
    for (let i = 0; i < 8; ++i) {
      const p = .8 / o[i], r = i === row.w ? o[i] : 0;
      let k = 0;
      while (k < EDGES.length - 2 && p >= EDGES[k + 1]) ++k;
      for (const g of [bins[k], ranks[rk.indexOf(i)]]) { g.n++; g.p += p; g.w += i === row.w; g.r += r; g.r2 += r * r; }
      ev += r;
    }
  });
  const fin = g => { const e = g.r / g.n, se = Math.sqrt((g.r2 / g.n - e * e) / g.n); return {n: g.n, pred: g.p / g.n, obs: g.w / g.n, ev: e, se}; };
  return {bins: bins.map(fin), ranks: ranks.map(fin), all: ev / rows.length / 8};
}
const q = (a, p) => { const b = a.slice().sort((x, y) => x - y); return b[Math.floor(p * (b.length - 1))]; };
const pc = v => (100 * v).toFixed(1);

function summary(rows, label) {
  const N = rows.length, lane = Array(8).fill(0), sty = [0, 0, 0];
  for (const r of rows) lane[r.w]++, sty[r.sty]++;
  const cal = calib(rows, r => r.od), mean = k => rows.reduce((q, r) => q + r[k], 0) / N;
  const lanes = lane.map(c => c / N), se = Math.sqrt(.125 * .875 / N);
  return {label, N, lanes, laneSpread: Math.max(...lanes) - Math.min(...lanes), laneSE: se, sty: sty.map(c => c / N),
    fav: cal.ranks[0].obs, ranksObs: cal.ranks.map(g => g.obs), ranksEV: cal.ranks.map(g => g.ev), binsEV: cal.bins.map(g => g.ev), evAll: cal.all,
    mgMed: q(rows.map(r => r.mg), .5), nose: rows.filter(r => r.mg < .12).length / N, head: rows.filter(r => r.mg < .6).length / N,
    over24: rows.filter(r => r.mg > 2.4).length / N,
    tP10: q(rows.map(r => r.t), .1), tMed: q(rows.map(r => r.t), .5), tP90: q(rows.map(r => r.t), .9),
    swBend: mean('swBend'), swAll: mean('swAll'), lcBend: mean('lcBend'), lcLast: mean('lcLast'),
    lcBendNone: rows.filter(r => !r.lcBend).length / N};
}
function print(S) {
  console.log(`== ${S.label}  (${S.N} races)`);
  console.log(`  lane win % (0 = rail): ${S.lanes.map(pc).join(' ')}   spread ${pc(S.laneSpread)} pt (1 SE ${pc(S.laneSE)})`);
  console.log(`  style FRONT/STALK/CLOSE: ${S.sty.map(pc).join(' / ')}`);
  console.log(`  win % by popularity: ${S.ranksObs.map(pc).join(' ')}`);
  console.log(`  EV (shipped odds) by popularity: ${S.ranksEV.map(v => v.toFixed(3)).join(' ')}  overall ${S.evAll.toFixed(3)}`);
  console.log(`  EV by chance bin ${EDGES.slice(1).join('/')}: ${S.binsEV.map(v => v.toFixed(3)).join(' ')}`);
  console.log(`  margin median ${S.mgMed.toFixed(2)} m, nose ${pc(S.nose)}%, head-or-closer ${pc(S.head)}%, over 2.4 m ${pc(S.over24)}%`);
  console.log(`  winner time p10/med/p90 ${S.tP10.toFixed(2)} / ${S.tMed.toFixed(2)} / ${S.tP90.toFixed(2)} s`);
  console.log(`  rank swaps per race: bend ${S.swBend.toFixed(1)}, all ${S.swAll.toFixed(1)}; lead changes: bend ${S.lcBend.toFixed(2)} (none ${pc(S.lcBendNone)}%), last 300 m ${S.lcLast.toFixed(2)}`);
}

async function main() {
  const args = process.argv.slice(2), cmd = args[0], mode = (args.find(v => v.startsWith('--mode=')) || '--mode=x').slice(7);
  const pos = args.slice(1).filter(v => !v.startsWith('--'));
  if (cmd === 'verify') {
    // The patched step() with no s.cm must be the shipped step(), every digit.
    const N = +(pos[0] || 2000), d0 = load(SRC), dx = patched('x'), dg = patched('g');
    let bad = 0;
    for (let n = 0; n < N; ++n) {
      const f = d0.field(seedOf(n)), a = d0.race(f), b = dx.race(f), c = dg.race(f);
      while (a.done < 8 && a.t < 200) {
        d0.step(f, a, d0.DT); dx.step(f, b, dx.DT); dg.step(f, c, dg.DT);
        for (let i = 0; i < 8; ++i) if (a.x[i] !== b.x[i] || a.x[i] !== c.x[i] || a.v[i] !== b.v[i] || a.v[i] !== c.v[i] || a.e[i] !== b.e[i] || a.e[i] !== c.e[i]) bad++;
      }
      for (let i = 0; i < 8; ++i) if (a.tc[i] !== b.tc[i] || a.tc[i] !== c.tc[i]) bad++;
    }
    console.log(`verify ${N} seeds: ${bad ? 'CORNER_VERIFY FAIL ' + bad : 'CORNER_VERIFY PASS (x, v, e every step and tc equal to the shipped step)'}`);
    process.exit(bad ? 1 : 0);
  }
  if (cmd === 'sweep') {
    const N = +(pos[0] || 20000);
    const sets = (pos[1] || '0,0;.0025,0;.005,0;.01,0;0,.005;0,.01;0,.02;.0025,.005;.005,.01;.01,.02').split(';').map(v => v.split(',').map(Number));
    const all = [];
    for (const [a, s] of sets) {
      const S = summary(await runPar(N, a, s, mode), `a ${(a * 100).toFixed(2)}% s ${(s * 100).toFixed(2)}% mode ${mode}`);
      print(S);
      all.push({a, s, mode, ...S});
    }
    if (args.includes('--json')) console.log('JSON ' + JSON.stringify(all));
    return;
  }
  if (cmd === 'fit') {
    const N = +(pos[0] || 40000), a = +(pos[1] || 0), s = +(pos[2] || 0), rows = await runPar(N, a, s, mode);
    // Fit on the even races, check on the odd ones (held out).
    const tr = rows.filter((_, n) => n % 2 === 0), te = rows.filter((_, n) => n % 2 === 1);
    const f5 = fit(tr, 5), f6 = fit(tr, 6);
    console.log(`fit a ${a} s ${s} mode ${mode}, ${tr.length} races: ` + FEAT.map((f, k) => f + ' ' + f6.b[k].toFixed(3)).join(', ') +
      `; mean LL ${f6.L.toFixed(4)} (without lane ${f5.L.toFixed(4)}: ` + FEAT.slice(0, 5).map((f, k) => f + ' ' + f5.b[k].toFixed(3)).join(', ') + ')');
    const shipped = [17.2, 2.6, .5, .5, -2.3, 0];
    // Candidate weights: the fit rounded like the shipped ones (acc and sty>1
    // share one weight in odds(), so their mean is used).
    // The lane weight keeps two decimals (it is small); --c= tries other tails.
    const r1 = v => Math.round(v * 10) / 10, r2 = v => Math.round(v * 100) / 100, ac = r1((f6.b[2] + f6.b[3]) / 2);
    const cand = [r1(f6.b[0]), r1(f6.b[1]), ac, ac, r1(f6.b[4]), r2(f6.b[5])];
    const extra = (args.find(v => v.startsWith('--w=')) || '').slice(4);
    const cs = (args.find(v => v.startsWith('--c=')) || '--c=.0015').slice(4).split('/').map(Number);
    const tries = [['shipped odds', shipped, .0015], ['shipped + lane', [...shipped.slice(0, 5), r2(f6.b[5])], .0015]];
    for (const c of cs) tries.push(['refit', cand, c]);
    if (extra) for (const c of cs) tries.push(['--w', extra.split('/').map(Number), c]);
    for (const [name, w, c] of tries) {
      const C = calib(te, r => oddsW(r, w, c));
      const evs = C.bins.concat(C.ranks).filter(g => g.n > 0).map(g => g.ev);
      console.log(`  ${name.padEnd(15)} w [${w.join(', ')}] c ${c}: overall ${C.all.toFixed(3)}, EV range ${Math.min(...evs).toFixed(3)}..${Math.max(...evs).toFixed(3)}`);
      console.log(`    ranks pred->obs EV: ` + C.ranks.map(g => `${pc(g.pred)}->${pc(g.obs)} ${g.ev.toFixed(2)}`).join(' | '));
      console.log(`    bins EV: ` + C.bins.map(g => `${g.ev.toFixed(3)}±${g.se.toFixed(3)}`).join(' '));
    }
    return;
  }
  if (cmd === 'public') {
    // Odds from what the bettor sees before betting: the paddock figures,
    // the course kind and the gate (the tote lists NO.1-8 by lane). The
    // bend's per-runner draw never enters. (a) straight odds as shipped;
    // (b) + a lane term, its weight the only thing fitted; (c) (b) with a
    // temperature g on the whole score and the take k refitted (k sets the
    // overall return back to 0.8). Fit on even races, measured on odd.
    const N = +(pos[0] || 80000), a = +(pos[1] || 0), s = +(pos[2] || 0), rows = await runPar(N, a, s, mode);
    const tr = rows.filter((_, n) => n % 2 === 0), te = rows.filter((_, n) => n % 2 === 1);
    const U0 = r => r.h.map(k => 17.2 * k[0] + 2.6 * k[1] - 2.3 * k[4] + .5 * (k[3] + k[2]));
    const LN = r => r.h.map((_, i) => [(3.5 - i) / 3.5]);
    const fb = fit(tr, 1, LN, U0), lam = Math.round(fb.b[0] * 100) / 100;
    const fc = fit(tr, 2, r => U0(r).map((u, i) => [u, (3.5 - i) / 3.5]));
    const gam = Math.round(fc.b[0] * 100) / 100, lamc = Math.round(fc.b[1] * 100) / 100;
    const oddsS = (u, k, c) => {
      const e = u.map(v => Math.exp(v)), z = e.reduce((q, v) => q + v);
      return e.map(v => { const q = z / v; return Math.max(1.1, Math.min(999, Math.round(10 * k * q * (1 + c * q)) / 10)); });
    };
    const Sb = r => U0(r).map((u, i) => u + lam * (3.5 - i) / 3.5), Sc = r => U0(r).map((u, i) => gam * u + lamc * (3.5 - i) / 3.5);
    // The take: k so that the training half returns 0.8 overall.
    let lo = .5, hi = 1.2;
    for (let it = 0; it < 30; ++it) { const k = (lo + hi) / 2; calib(tr, r => oddsS(Sc(r), k, .0015)).all > .8 ? hi = k : lo = k; }
    const kc = Math.round((lo + hi) / 2 * 1000) / 1000;
    console.log(`public odds a ${a} s ${s}: (b) lane ${lam} (LL ${fb.L.toFixed(4)}); (c) temperature ${gam}, lane ${lamc}, take k ${kc} (LL ${fc.L.toFixed(4)})`);
    const out = {a, s, lam, gam, lamc, kc};
    for (const [name, od] of [['a', r => oddsS(U0(r), .8, .0015)], ['b', r => oddsS(Sb(r), .8, .0015)], ['c', r => oddsS(Sc(r), kc, .0015)]]) {
      const C = calib(te, od), gs = C.bins.concat(C.ranks).filter(g => g.n), dev = Math.max(...gs.map(g => Math.abs(g.ev - .8)));
      out[name] = {ranks: C.ranks.map(g => [g.pred, g.obs, g.ev, g.se]), bins: C.bins.map(g => [g.pred, g.obs, g.ev, g.se, g.n]), all: C.all, dev};
      console.log(`  (${name}) overall ${C.all.toFixed(3)}, max |EV-0.8| ${dev.toFixed(3)}`);
      console.log(`    ranks EV: ` + C.ranks.map(g => g.ev.toFixed(3)).join(' '));
      console.log(`    bins EV:  ` + C.bins.map(g => g.n ? g.ev.toFixed(3) : '-').join(' '));
    }
    if (args.includes('--json')) console.log('JSON ' + JSON.stringify(out));
    return;
  }
  console.log('usage: derby_corner.mjs verify [seeds] | sweep [races] [a,s;...] [--mode=x|g] | fit [races] a s [--mode=x|g]');
}
