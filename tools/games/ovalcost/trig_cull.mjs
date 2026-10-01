// Host count of DERBY WATCH's panning frames for docs/apps/derby-trig-cull.md:
// pose() calls, distinct g, Math.sin/cos by caller, the chord bodies of ser()
// and their size on the panel, and what the trig and culling variants
// (variants.mjs) save. It runs the app's own pose()/wide()/shot()/pan()/ser()
// (the working tree, or --ref <git ref>) with dr() and Math wrapped by
// counters. Counts are exact for the scripted frames (host, measured); the
// microseconds are counts x the device's unit prices (estimates).
//
//   node tools/games/ovalcost/trig_cull.mjs [--ref vm/main] [--tier 1] [--step 5] [--json out.json]
import { readFileSync, writeFileSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { VARIANTS, patchApp } from './variants.mjs';

const arg = (k, d) => { const i = process.argv.indexOf(k); return i < 0 ? d : process.argv[i + 1]; };
const REF = arg('--ref', ''), TIER = +arg('--tier', 1), STEP = +arg('--step', 5), JSON_OUT = arg('--json', '');
const ROOT = new URL('../../../', import.meta.url);
const show = p => REF ? execFileSync('git', ['show', `${REF}:${p}`], { encoding: 'utf8' }) : readFileSync(new URL(p, ROOT), 'utf8');

// Device unit prices, microseconds. docs/apps/derby-pan-camera-cost.md section 1
// (JS ops, measured), docs/apps/derby-pan-memory.md section 3 "実機の内訳"
// (pose 135 us on a bend, 26 us on the straight's shortcut; Math.sin 18.7 us
// inside DERBY; a ser() body 1.2-1.5 ms; all measured on the device).
export const P = { add: 2.27, mul: 2.50, div: 4.87, rd: 2.75, wr: 3.77, f32rd: 2.75, f32wr: 4.25, call: 4.44, prop: 2.18,
  trig: 18.7, trigLo: 12.1, sqrt: 7.42, arr4: 14, arr8: 22.7, drawFixed: 37.3, step: 0.46, sinExtra: 1.45,
  poseBend: 135, poseStraight: 26, serBodyLo: 1200, serBodyHi: 1500 };

function makeApp(variant) {
  const files = patchApp({ view: show('apps/derby/derby_view.js'), pan: show('apps/derby/derby_pan.js'),
    scene: show('apps/derby/derby_scene.js') }, variant);
  const view = files.view, scene = files.scene;
  // Counting only: each chord body that reaches the LOD loop reports itself.
  const pan = files.pan.replace('    if (!(Lo < Hi)) continue;\n', '    if (!(Lo < Hi)) continue;\n    __s.onBody(n, w, j, ga, gb, q, u, v, Lo, Hi, s, b);\n');
  if (pan === files.pan) throw new Error('trig_cull.mjs: ser() body hook not found');
  const cut = view.slice(view.indexOf('const VS ='), view.indexOf('// Draws one frame of the course'));
  // paint()'s screen poses and vr/vq, as derby_scene.js has them.
  const pc0 = scene.indexOf('  if (pc) {', scene.indexOf('function paint(')), pa = scene.lastIndexOf('\n  const a = ', pc0) + 1, pb = scene.indexOf('  } else {', pa);
  const paint = scene.slice(pa, pb) + '}';
  // A variant's once-a-race table (enter('pad') in derby_scene.js), if any.
  const race = scene.split('\n').filter(l => /^\s+CP = new/.test(l)).join('\n');
  const C = {}, bump = (k, n = 1) => { C[k] = (C[k] || 0) + n; };
  const st = { who: 'other', gs: new Map(), poses: [], bodies: [], draws: [] };
  const M = {};
  for (const k of Object.getOwnPropertyNames(Math)) M[k] = typeof Math[k] === 'function' ? (...a) => {
    bump('M.' + k); if (k === 'sin' || k === 'cos') bump('trig.' + st.who); return Math[k](...a); } : Math[k];
  const sin = x => { bump('M.sin'); bump('trig.' + st.who); return Math.sin(x); };
  const env = {
    M, PI: Math.PI, sin, flo: x => { bump('M.floor'); return Math.floor(x); }, rnd: x => { bump('M.round'); return Math.round(x); },
    mx: (...a) => { bump('M.max'); return Math.max(...a); }, mn: (...a) => { bump('M.min'); return Math.min(...a); },
    D: 1000, DT: .05, U: 1 / 6, DNR: 11, DFR: 22.6, DL: Array.from({ length: 8 }, (_, j) => 12 * 1.085 ** j),
    KN: [[3, 2, 3, 3, 8, 10], [4, 3, 4, 5, 5, 7], [5, 4, 6, 8, 4, 5]], tier: TIER, t: 0, vr: null, von: 11, ph: [0, 1, 2, 3, 4, 5, 6, 7],
    HX: 92, HY: 88, HS: 4, cx: 0, rs: { v: [0] }, scene: 'race', cm: 8,
    dr: (n, a) => { bump('dr'); bump('dr.' + n); st.draws.push([n, a.slice(), st.who, st.body]); },
    rin: (l, X, S, Y) => { bump('rin'); st.draws.push(['runner', [X, S, Y], 'runner', -1]); }, feed: () => {},
  };
  const names = Object.keys(env);
  const body = `'use strict'; let ${names.map(n => `${n} = __e.${n}`).join(', ')};\n${cut}\n${pan.replace("'use strict';", '')}
    function __paint() {\n${paint}\n  if (scene !== 'race' || cm === 6) vr = null; }
    const __pose = pose, __ser = ser, __wide = wide, __shot = shot;
    pose = function (g, w) { const k = __s.tc(); const r = __pose(g, w); __s.onPose(g, w, r, __s.tc() - k); return r; };
    ser = function (n, w) { const o = __s.who; __s.who = 'ser:' + n + '@' + w; __s.serName = n; __s.serW = w;
      const r = __ser.apply(null, arguments); __s.who = o; return r; };
    wide = function () { const o = __s.who; __s.who = 'wide'; const r = __wide.apply(null, arguments); __s.who = o; return r; };
    shot = function () { const o = __s.who; __s.who = 'shot'; const r = __shot.apply(null, arguments); __s.who = o; return r; };
    if (typeof dv === 'function') { const __dv = dv; dv = function () { __s.bump('dv'); return __dv.apply(null, arguments); }; }
    if (typeof dev === 'function') { const __dev = dev; dev = function () { __s.bump('dev'); return __dev.apply(null, arguments); }; }
    return {
      setCourse(o, w2) { CRS = o ? OC : SC; CH = [-1e4]; if (o) for (let j = 0; j <= PAN[3]; ++j) CH.push(OB + j * 120 * PI / PAN[3]); CH.push(1e4);
        CAMS[8][5] = w2;
${race}
      },
      frame(xs, m) { const L = mx.apply(null, xs); if (!m) m = wide(L); if (!m) return 0; cm = m; shot(m, xs, 0, 0);
        __s.who = 'screen'; __paint(); __s.who = 'pan'; pan(xs); __s.who = 'other'; return m; },
      get: () => ({ pc, VC, vr, CH, OB, CRS, OC, PAN, CAMS }), OB, pose: __pose };`;
  st.onPose = (g, w, r, k) => { bump('pose'); bump('pose.' + st.who); st.poses.push([st.who, g, w, r, k]); };
  st.bump = bump;
  st.tc = () => (C['M.sin'] || 0) + (C['M.cos'] || 0);
  st.body = -1;
  st.onBody = (n, w, j, ga, gb, q, u, v, Lo, Hi, s, b) => { st.body = st.bodies.length; st.bodies.push({ n, w, j, ga, gb, q: q.slice(), u, v, Lo, Hi, s, b }); };
  const app = new Function('__e', '__s', body)(env, st);
  return { app, C, st, env };
}

// The caller of a pose, for the tables: ser's series by name and depth, pan's
// own loops by what they place.
function caller(who, w, g) {
  if (who.startsWith('ser:')) { const [n, d] = who.slice(4).split('@'); return n === 'prail' ? (+d === 40 ? 'ser 柵(スタンド)' : +d === 11 ? 'ser 内柵' : 'ser 外柵') : n === 'pk' ? 'ser 観客' : 'ser 芝'; }
  if (who === 'pan') return w === 0 ? 'pan() 弦の端(VE)' : g % 200 === 0 && (w === 22.6 || w === 11) ? '距離標' : '走者';
  return { wide: 'wide()', shot: 'shot()', screen: '大型画面', other: 'other' }[who] || who;
}

const range = (a, b, s) => { const r = []; for (let x = a; x <= b + 1e-9; x += s) r.push(x); return r; };

// One configuration over leaders L: per frame counts, the caller split, the
// chord bodies (each ser() chord that reaches the LOD loop) with their panel
// box, and the draws.
export function runSet(variant, oval, w2, leaders, cam) {
  const { app, C, st } = makeApp(variant);
  app.setCourse(oval, w2);
  const out = [];
  for (const L of leaders) {
    for (const k in C) delete C[k];
    st.poses.length = 0; st.draws.length = 0; st.bodies.length = 0; st.body = -1;
    const xs = Array.from({ length: 8 }, (_, i) => L - 1.5 * i);
    const m = app.frame(xs, cam);
    if (!m) continue;
    out.push({ L, m, C: { ...C }, poses: st.poses.slice(), draws: st.draws.slice(), bodies: st.bodies.slice(), g: app.get() });
  }
  return { out, app };
}

// The bend's interval of g (pose's section 1).
const OB = 650 - 120 * Math.PI;
const onBend = g => g > OB && g <= 650;

function summarize(frames) {
  const n = frames.length || 1, S = {};
  const add = (k, v) => { S[k] = (S[k] || 0) + v; };
  for (const f of frames) {
    add('pose', f.poses.length); add('trig', (f.C['M.sin'] || 0) + (f.C['M.cos'] || 0));
    add('poseBend', f.poses.filter(p => onBend(p[1])).length);
    add('distinctG', new Set(f.poses.map(p => p[1])).size);
    add('distinctGBend', new Set(f.poses.filter(p => onBend(p[1])).map(p => p[1])).size);
    add('distinctGW', new Set(f.poses.map(p => p[1] + ',' + p[2])).size);
    add('dr', f.C.dr || 0);
    for (const p of f.poses) { const c = caller(p[0], p[2], p[1]); add('pose:' + c, 1); add('trig:' + c, p[4]); }
  }
  for (const k in S) S[k] /= n;
  return S;
}

if (import.meta.url === `file://${process.argv[1].replace(/\\/g, '/').replace(/^([A-Za-z]):/, '/$1:')}` || process.argv[1].endsWith('trig_cull.mjs')) {
  const L = range(0, 1000, STEP);
  const regions = { 'バックストレッチ': x => x < OB, 'コーナー': x => x >= OB && x < 650, 'ホームストレッチ': x => x >= 650 };
  const res = {};
  console.log(`ref ${REF || 'working tree'}, tier ${TIER}, leader 0..1000 m every ${STEP} m; bend ${OB.toFixed(1)}..650 m`);
  for (const [cfg, oval, w2] of [['楕円 w=-100', true, -100], ['楕円 w=-14', true, -14], ['直線', false, -100]]) {
    for (const cam of [0, 7, 8, 9]) {
      const { out } = runSet(null, oval, w2, L, cam);
      for (const [rn, rf] of Object.entries(oval ? regions : { '直線': () => true })) {
        const fr = out.filter(f => rf(f.L));
        if (!fr.length) continue;
        const S = summarize(fr), key = `${cfg} | ${cam ? 'WIDE ' + (cam - 6) : '監督(wide())'} | ${rn}`;
        res[key] = { frames: fr.length, ...S };
        const parts = Object.entries(S).filter(([k]) => k.startsWith('pose:')).map(([k, v]) => `${k.slice(5)} ${v.toFixed(1)}`).join(', ');
        const tparts = Object.entries(S).filter(([k]) => k.startsWith('trig:')).map(([k, v]) => `${k.slice(5)} ${v.toFixed(1)}`).join(', ');
        console.log(`${key}: ${fr.length} frames; pose ${S.pose.toFixed(1)} (曲線上 ${S.poseBend.toFixed(1)}), 異なる g ${S.distinctG.toFixed(1)} (曲線上 ${S.distinctGBend.toFixed(1)}), 異なる (g,w) ${S.distinctGW.toFixed(1)}, sin+cos ${S.trig.toFixed(1)}, dr ${S.dr.toFixed(1)}`);
        console.log(`    pose by caller: ${parts}`);
        console.log(`    trig by caller: ${tparts}`);
      }
    }
  }
  if (JSON_OUT) writeFileSync(JSON_OUT, JSON.stringify(res, null, 1));
}

// ---- Chord bodies: each one's box on the panel, its draws, VM steps and
// segments (the plans' loops, derby_prog.js), for the culling tables.
const HEIGHT = b => b.n === 'prail' ? (b.b < 0 ? 13.5 : 1.1) : b.n === 'pk' ? 7.2 : 0;
export function bodyBox(b) {
  const pt = g => { const Z = b.q[1] + g * b.u; return [(b.q[0] + g * b.v) / Z, 28 + 6 / Z, 28 + (6 - HEIGHT(b)) / Z, Z]; };
  const A = pt(b.Lo), B = pt(b.Hi), xs = [A[0], B[0]], ys = [A[1], A[2], B[1], B[2]];
  const dx = Math.abs(B[0] - A[0]), dy = Math.max(...ys) - Math.min(...ys);
  return { x0: Math.min(...xs), x1: Math.max(...xs), y0: Math.min(...ys), y1: Math.max(...ys), dx, dy, size: Math.max(dx, dy), zNear: Math.min(A[3], B[3]), zFar: Math.max(A[3], B[3]) };
}
export function drawCost(n, a, tier = TIER) {
  // [VM steps, sin calls, segments or dots]; Newton 3 x 3 steps a point (oval_cost.mjs).
  const c = a[4];
  if (n === 'prail') return [34 + c * 19, 0, 2 * c + 2];
  if (n === 't0' || n === 't1') return [36 + c * 34, 0, c];
  if (n === 'pk') { const rows = [[3, 2, 3, 3, 8, 10], [4, 3, 4, 5, 5, 7], [5, 4, 6, 8, 4, 5]][tier][1], dots = rows / 2; return [20 + c * (20 + 11 * dots), c * dots, c * dots]; }
  if (n === 'hl') return [22 + a[6] * 8, 0, a[6] + 2];
  return [40, 0, 6];
}
// A body's device time, estimated: the JS of the body (oval_cost.mjs's hand
// count, 290 us), a pose when the chord's first end is not the last one's
// (135 us on the bend), each draw's 8-array and expressions (22.7 + 60) and
// fixed cost (37.3), and its VM steps (0.46 us) and sin (1.45 us extra).
export function bodyCost(b, draws, poses) {
  let us = 290 + poses * P.poseBend;
  for (const [n, a] of draws) { const s = drawCost(n, a); us += P.arr8 + 60 + P.drawFixed + s[0] * P.step + s[1] * P.sinExtra; }
  return us;
}
