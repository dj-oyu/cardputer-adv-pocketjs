// Host count of what one panning frame of DERBY WATCH's oval does in JS, and a
// cost model built from the device's unit prices (docs/kasane/oval-pie-design.md
// section 2). It runs the real pose()/wide()/shot()/pan()/ser() of a branch
// (default vm/oval, read with `git show`), with dr() and the Math functions
// wrapped by counters. Nothing here is a device measurement: the counts are
// exact for the scripted frames, the microseconds are counts x unit prices.
//
//   node tools/games/ovalcost/oval_cost.mjs [--ref vm/oval] [--tier 1]
import { execFileSync } from 'node:child_process';

const arg = (k, d) => { const i = process.argv.indexOf(k); return i < 0 ? d : process.argv[i + 1]; };
const REF = arg('--ref', 'vm/oval'), TIER = +arg('--tier', 1);
const show = p => execFileSync('git', ['show', `${REF}:${p}`], { encoding: 'utf8' });
const view = show('apps/derby/derby_view.js'), pan = show('apps/derby/derby_pan.js');
// The course, pose(), the cameras, wide() and shot(): from the screen's
// constants to the comment before course().
const cut = view.slice(view.indexOf('const VS ='), view.indexOf('// Draws one frame of the course'));

// Device unit prices, microseconds (docs/apps/derby-pan-camera-cost.md section 1).
const P = { trig: 12.5, sqrt: 7.4, arith: 2.4, div: 4.9, cmp: 3.2, rd: 2.75, call: 4.4, arr8: 22.7, arr4: 14, arr2: 10,
  drawFixed: 37.3, step: 0.45, sinExtra: 1.45 };

const C = {};
const bump = (k, n = 1) => { C[k] = (C[k] || 0) + n; };
const realM = Math, M = {};
for (const k of Object.getOwnPropertyNames(Math)) {
  M[k] = typeof Math[k] === 'function' ? (...a) => { bump('M.' + k); return realM[k](...a); } : Math[k];
}
const sin = x => { bump('M.sin'); return realM.sin(x); };
const draws = [];
const env = {
  M, PI: Math.PI, sin, flo: x => { bump('M.floor'); return realM.floor(x); }, rnd: x => { bump('M.round'); return realM.round(x); },
  mx: (...a) => { bump('M.max'); return realM.max(...a); }, mn: (...a) => { bump('M.min'); return realM.min(...a); },
  D: 1000, DT: .05, U: 1 / 6, DNR: 11, DFR: 22.6, DL: Array.from({ length: 8 }, (_, j) => 12 * 1.085 ** j),
  KN: [[3, 2, 3, 3, 8, 10], [4, 3, 4, 5, 5, 7], [5, 4, 6, 8, 4, 5]], tier: TIER, t: 0, vr: null, von: 0, ph: [0, 1, 2, 3, 4, 5, 6, 7],
  HX: 92, HY: 88, HS: 4, cx: 0, rs: { v: [0] },
  dr: (n, a) => { bump('dr'); bump('dr.' + n); draws.push([n, a]); },
  rin: () => bump('rin'), feed: () => {},
};
const names = Object.keys(env);
const body = `'use strict'; let ${names.map(n => `${n} = __e.${n}`).join(', ')};\n${cut}\n${pan.replace("'use strict';", '')}
  const __w = (f, k) => function () { __b(k); return f.apply(null, arguments); };
  pose = __w(pose, 'pose'); pj = __w(pj, 'pj'); lim = __w(lim, 'lim'); inr = __w(inr, 'inr'); ser = __w(ser, 'ser');
  return { setCourse(o) { CRS = o ? OC : SC; CH = [-1e4]; if (o) for (let j = 0; j <= PAN[3]; ++j) CH.push(OB + j * 120 * PI / PAN[3]); CH.push(1e4); },
    frame(xs, m) { const L = mx.apply(null, xs); if (!m) m = wide(L); if (!m) return 0; shot(m, xs, 0, 0);
      const a = pose(VS[0] - VS[2], VS[1]), b = pose(VS[0] + VS[2], VS[1]); pj(a); pj(b); pan(xs); return m; },
    vc: () => VC.length / 2, OB, pc: () => pc };`;
const app = new Function('__e', '__b', body)(env, bump);

// VM steps of one draw, from the plans' loops (derby_prog.js; Newton 3 x 3
// steps a point). rcp: with a reciprocal instruction instead (1 step).
function steps(n, a, rcp) {
  const c = a[4], N = rcp ? 1 : 9;
  if (n === 'prail') return [34 + c * (10 + N), 0, 2 * c + 2];
  if (n === 't0' || n === 't1') return [24 + (rcp ? 1 : 12) + c * (13 + 2 * N + (rcp ? 0 : 3)), 0, c];
  if (n === 'pk') { const rows = env.KN[TIER][1], dots = rows / 2; return [20 + c * (11 + N + 11 * dots), c * dots, c * dots]; }
  if (n === 'hl') return [22 + a[6] * 8, 0, a[6] + 2];
  return [40, 0, 6];
}
function model(rcp) {
  let st = 0, sn = 0, seg = 0;
  for (const [n, a] of draws) { const s = steps(n, a, rcp); st += s[0]; sn += s[1]; seg += s[2]; }
  return { st, sn, seg };
}
// JS microseconds from the counters. pose's and ser's own arithmetic are hand
// counts of the source (the document lists them); trig and sqrt are counted.
function cost(c, frames, bodies) {
  const trig = (c['M.sin'] || 0) + (c['M.cos'] || 0), other = ['M.floor', 'M.round', 'M.max', 'M.min', 'M.abs', 'M.ceil']
    .reduce((s, k) => s + (c[k] || 0), 0);
  const j = {
    trig: trig * P.trig,
    sqrt: (c['M.sqrt'] || 0) * P.sqrt,
    mathCalls: other * (P.call + P.arith),
    poseOwn: (c.pose || 0) * 120,          // section loop, reads, the 4-array (hand count, no trig)
    pj: (c.pj || 0) * 60,
    lim: (c.lim || 0) * 20,
    inr: (c.inr || 0) * 30,
    serOwn: bodies.kept * 290 + bodies.culled * 176 - (c['M.sqrt'] || 0) * P.sqrt,
    drawArgs: (c.dr || 0) * (P.arr8 + 60),  // the 8-array and its expressions, the LOD turn around it
    drawFixed: (c.dr || 0) * P.drawFixed,
  };
  for (const k in j) j[k] /= frames;
  return j;
}

function run(label, oval, leaders, m) {
  app.setCourse(oval);
  for (const k in C) delete C[k];
  let frames = 0, vc = 0, st = 0, sn = 0, seg = 0, st2 = 0, kept = 0, culled = 0, maxDraw = 0;
  for (const L of leaders) {
    draws.length = 0;
    const b0 = C.ser || 0;
    const xs = Array.from({ length: 8 }, (_, i) => L - 1.5 * i);
    if (!app.frame(xs, m)) continue;
    ++frames; vc += app.vc();
    const a = model(false), b = model(true);
    st += a.st; sn += a.sn; seg += a.seg; st2 += b.st;
    // A body that reaches the LOD loop draws at least once for a rail.
    const nb = app.vc() * 6;
    const k = new Set(draws.filter(d => d[0] !== 'hl' && d[0] !== 'pole').map(d => d[0] + d[1][0])).size;
    kept += Math.min(nb, draws.length); culled += Math.max(0, nb - draws.length);
    maxDraw = Math.max(maxDraw, draws.length);
  }
  const per = k => ((C[k] || 0) / frames).toFixed(1);
  const j = cost(C, frames, { kept, culled });
  const js = Object.values(j).reduce((a, b) => a + b, 0);
  const vm = (st * P.step + sn * P.sinExtra) / frames, vm2 = (st2 * P.step + sn * P.sinExtra) / frames;
  console.log(`\n== ${label}: ${frames} frames, chords in view ${(vc / frames).toFixed(1)}`);
  console.log(`calls/frame: pose ${per('pose')} pj ${per('pj')} lim ${per('lim')} inr ${per('inr')} dr ${per('dr')} (max ${maxDraw})` +
    ` sin+cos ${((C['M.sin'] + (C['M.cos'] || 0)) / frames).toFixed(1)} sqrt ${per('M.sqrt')}`);
  console.log(`draws/frame: prail ${per('dr.prail')} pk ${per('dr.pk')} t0+t1 ${((C['dr.t0'] || 0) + (C['dr.t1'] || 0)) / frames} hl ${per('dr.hl')} pole ${per('dr.pole')}`);
  console.log(`trig per pose: ${((C['M.sin'] + (C['M.cos'] || 0)) / C.pose).toFixed(2)} (includes rin's none: rin is stubbed)`);
  console.log('JS model (us/frame): ' + Object.entries(j).map(([k, v]) => `${k} ${v.toFixed(0)}`).join(', '));
  console.log(`JS pan total ${(js / 1e3).toFixed(1)} ms; VM ${st / frames | 0} steps = ${(vm / 1e3).toFixed(2)} ms (with a reciprocal op ${st2 / frames | 0} steps = ${(vm2 / 1e3).toFixed(2)} ms); segments ${seg / frames | 0}`);
  // What-ifs (section 6): P0 pose() from stored section starts (2 trig calls on
  // the bend, none on a straight, 45 us of its own); P1 also the chord ends,
  // the chords' middles and the fixed points from tables (12 poses a frame left,
  // 10 us a chord body for the reads).
  const poses = C.pose / frames, tp = oval && label.includes('bend') ? 2 : oval ? .75 : 0;
  const p0 = js - j.trig - j.poseOwn + poses * (tp * P.trig + 45), p1 = js - j.trig - j.poseOwn + 12 * (tp * P.trig + 45) + (kept + culled) / frames * 10;
  console.log(`what-if pan JS: P0 ${(p0 / 1e3).toFixed(1)} ms, P0+P1 ${(p1 / 1e3).toFixed(1)} ms (chord bodies ${((kept + culled) / frames).toFixed(1)}/frame)`);
  return { js, vm, vm2, j };
}

const range = (a, b, s) => { const r = []; for (let x = a; x <= b; x += s) r.push(x); return r; };
const OB = app.OB;
console.log(`ref ${REF}, tier ${TIER}, bend ${OB.toFixed(1)}..650 m`);
run('straight course, WIDE 2 (unit at 460 m), leader 300..620', false, range(300, 620, 5), 8);
run('oval, back straight, WIDE 1 (unit at 100 m), leader 150..270', true, range(150, 270, 5), 7);
run('oval, bend, nearest unit, leader OB..650', true, range(Math.ceil(OB), 648, 3), 0);
run('oval, home straight, WIDE 3 (unit at 820 m), leader 660..960', true, range(660, 960, 5), 9);
