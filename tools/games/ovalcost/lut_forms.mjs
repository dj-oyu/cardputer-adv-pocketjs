// Ways to hold a cos/sin look-up table (and the per-race pose table) for
// DERBY WATCH's oval, compared on the host (docs/apps/derby-trig-cull.md
// section 5): for each form and size,
//   - the error against Math.cos/sin over every bend pose() the panning frames
//     make (m, and px on the panel), computed in Node with the same source;
//   - the guest bytes it keeps (table + its look-up function), measured with
//     the device-sized QuickJS (the harness built by run_derby.py --m32,
//     DERBY_EVAL_ONLY: the app evaluated with the form appended to a chunk,
//     against the app with a Math.cos/sin look-up of the same shape);
//   - the time of a look-up relative to Math.cos + Math.sin in that same
//     QuickJS on x86 (host, not the device: the device's doubles are
//     soft-float, the host's are not);
//   - an estimate of the device time from the measured unit prices and the
//     look-up's operations counted by hand (estimate; typed-array reads are
//     not measured on the device: bench/lut.js measures them).
//
//   node tools/games/ovalcost/lut_forms.mjs            (Windows or WSL: errors, estimates)
//   node tools/games/ovalcost/lut_forms.mjs --guest    (WSL: also the m32 bytes and host times)
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { runSet, P } from './trig_cull.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
const GUEST = process.argv.includes('--guest');
const OB = 650 - 120 * Math.PI, DNR = 11;

// A form: build(N) is top-level source that leaves its table in a global;
// look is the body of __lk(a) for a in [0, pi], setting c and s; ops is the
// look-up's operations for the device estimate (rd: element reads, which for
// a typed array are priced as an Array's: not measured).
const Q = 'const K = N / PI;';
export const FORMS = {
  trig: { sizes: [0], build: () => '', look: 'c = M.cos(a); s = M.sin(a);', ops: { trig: 2, prop: 2 } },
  arr: { sizes: [64, 128, 256, 512], build: N => `const LT = []; for (let i = 0; i <= ${N}; ++i) LT.push(M.cos(i * PI / ${N}), M.sin(i * PI / ${N}));`,
    look: N => `const t = a * ${N / Math.PI}, i = t | 0, u = t - i, j = 2 * i, c0 = LT[j], s0 = LT[j + 1]; c = c0 + (LT[j + 2] - c0) * u; s = s0 + (LT[j + 3] - s0) * u;`,
    ops: { mul: 4, add: 9, rd: 4 }, bytes: 8 },
  f64: { sizes: [64, 128, 256, 512], build: N => `const LT = new Float64Array(${2 * N + 2}); for (let i = 0; i <= ${N}; ++i) { LT[2 * i] = M.cos(i * PI / ${N}); LT[2 * i + 1] = M.sin(i * PI / ${N}); }`,
    look: N => `const t = a * ${N / Math.PI}, i = t | 0, u = t - i, j = 2 * i, c0 = LT[j], s0 = LT[j + 1]; c = c0 + (LT[j + 2] - c0) * u; s = s0 + (LT[j + 3] - s0) * u;`,
    ops: { mul: 4, add: 9, rd: 4 }, bytes: 8 },
  f32: { sizes: [64, 128, 256, 512], build: N => `const LT = new Float32Array(${2 * N + 2}); for (let i = 0; i <= ${N}; ++i) { LT[2 * i] = M.cos(i * PI / ${N}); LT[2 * i + 1] = M.sin(i * PI / ${N}); }`,
    look: N => `const t = a * ${N / Math.PI}, i = t | 0, u = t - i, j = 2 * i, c0 = LT[j], s0 = LT[j + 1]; c = c0 + (LT[j + 2] - c0) * u; s = s0 + (LT[j + 3] - s0) * u;`,
    ops: { mul: 4, add: 9, rd: 4 }, bytes: 4 },
  // Q14 integers: reads are small ints (no double boxing), one scale at the end.
  i16: { sizes: [64, 128, 256, 512], build: N => `const LT = new Int16Array(${2 * N + 2}); for (let i = 0; i <= ${N}; ++i) { LT[2 * i] = M.round(16384 * M.cos(i * PI / ${N})); LT[2 * i + 1] = M.round(16384 * M.sin(i * PI / ${N})); }`,
    look: N => `const t = a * ${N / Math.PI}, i = t | 0, u = t - i, j = 2 * i, c0 = LT[j], s0 = LT[j + 1]; c = (c0 + (LT[j + 2] - c0) * u) * 6.103515625e-5; s = (s0 + (LT[j + 3] - s0) * u) * 6.103515625e-5;`,
    ops: { mul: 6, add: 9, rd: 4 }, bytes: 2 },
  // One sine table over [0, 3pi/2]: cos(a) = sin(a + pi/2), N/2 entries on.
  sin1: { sizes: [64, 128, 256, 512], build: N => `const LT = new Float32Array(${3 * N / 2 + 2}); for (let i = 0; i <= ${3 * N / 2 + 1}; ++i) LT[i] = M.sin(i * PI / ${N});`,
    look: N => `const t = a * ${N / Math.PI}, i = t | 0, u = t - i, k = i + ${N / 2}, s0 = LT[i], c0 = LT[k]; s = s0 + (LT[i + 1] - s0) * u; c = c0 + (LT[k + 1] - c0) * u;`,
    ops: { mul: 3, add: 9, rd: 4 }, bytes: 4 },
  // A quarter wave [0, pi/2]: the bend's angle a in [0, pi] folds once.
  qw: { sizes: [64, 128, 256, 512], build: N => `const LT = new Float32Array(${N / 2 + 2}); for (let i = 0; i <= ${N / 2 + 1}; ++i) LT[i] = M.sin(i * PI / ${N});`,
    // sin at t (or N - t past the middle), cos as sin at N/2 - t (or minus
    // sin at t - N/2): two interpolations at folded positions.
    look: N => `const t = a * ${N / Math.PI}, h = ${N / 2}, x = t < h ? t : ${N} - t, y = t < h ? h - t : t - h, i = x | 0, k = y | 0, s0 = LT[i], c0 = LT[k];
      s = s0 + (LT[i + 1] - s0) * (x - i); c = (c0 + (LT[k + 1] - c0) * (y - k)) * (t < h ? 1 : -1);`,
    ops: { mul: 4, add: 12, rd: 4, cmp: 3 }, bytes: 4 },
  // ---- Symmetry: one sine table over a quarter wave [0, pi/2] at N nodes a
  // half turn (spacing pi/N), the nearest node and a Taylor step; cos from
  // the same table by the swap cos(x) = sin(pi/2 - x), the second quarter by
  // sin(pi - x) = sin x, cos(pi - x) = -cos x. qt2: second order, qt3: third.
  qt2: { sizes: [32, 64], build: N => `const LT = []; for (let i = 0; i <= ${N / 2}; ++i) LT.push(M.sin(i * PI / ${N}));`,
    look: N => `const h2 = ${N / 2}, n = (a * ${N / Math.PI} + .5) | 0, h = a - n * ${Math.PI / N}, g = h * h * .5, k = n > h2 ? ${N} - n : n, S = LT[k], C = n > h2 ? -LT[h2 - k] : LT[h2 - k];
      c = C - h * S - g * C; s = S + h * C - g * S;`,
    ops: { mul: 8, add: 11, rd: 2, cmp: 3 }, bytes: 8 },
  qt3: { sizes: [16, 32], build: N => `const LT = []; for (let i = 0; i <= ${N / 2}; ++i) LT.push(M.sin(i * PI / ${N}));`,
    look: N => `const h2 = ${N / 2}, n = (a * ${N / Math.PI} + .5) | 0, h = a - n * ${Math.PI / N}, g = h * h * .5, e = g * h * .3333333333333333, k = n > h2 ? ${N} - n : n, S = LT[k], C = n > h2 ? -LT[h2 - k] : LT[h2 - k];
      c = C - h * S - g * C + e * S; s = S + h * C - g * S - e * C;`,
    ops: { mul: 12, add: 13, rd: 2, cmp: 3 }, bytes: 8 },
  // No table: fold to |x| <= pi/4 around 0 or pi/2 (cos/sin swap) and odd /
  // even Taylor polynomials to x^7 / x^8 (error < 4e-7 at pi/4).
  poly: { sizes: [0], build: () => '', look: () => `const q = a > 2.356194490192345 ? 2 : a > .7853981633974483 ? 1 : 0, x = a - q * 1.5707963267948966, x2 = x * x,
      ps = x * (1 + x2 * (-.16666666666666666 + x2 * (.008333333333333333 + x2 * -1.984126984126984e-4))), pc = 1 + x2 * (-.5 + x2 * (.041666666666666664 + x2 * (-.001388888888888889 + x2 * 2.48015873015873e-5)));
      if (q === 0) { c = pc; s = ps; } else if (q === 1) { c = -ps; s = pc; } else { c = -pc; s = -ps; }`,
    ops: { mul: 12, add: 11, cmp: 4 } },
  // ---- The per-race pose table (vetab's CP: 19 chord ends x [x, z, c, d]),
  // look-up = one entry's 4 values; error 0 for f64 and Array.
  ptab64: { sizes: [19], table: 1, build: () => `const LT = new Float64Array(76); for (let i = 0; i < 76; ++i) LT[i] = 650 + i * 1.37;`, look: () => `const j = 4 * (a | 0); c = LT[j] + LT[j + 1]; s = LT[j + 2] + LT[j + 3];`, ops: { rd: 4, add: 3 }, bytes: 8 },
  ptab32: { sizes: [19], table: 1, build: () => `const LT = new Float32Array(76); for (let i = 0; i < 76; ++i) LT[i] = 650 + i * 1.37;`, look: () => `const j = 4 * (a | 0); c = LT[j] + LT[j + 1]; s = LT[j + 2] + LT[j + 3];`, ops: { rd: 4, add: 3 }, bytes: 4 },
  ptabArr: { sizes: [19], table: 1, build: () => `const LT = []; for (let i = 0; i < 76; ++i) LT.push(650 + i * 1.37);`, look: () => `const j = 4 * (a | 0); c = LT[j] + LT[j + 1]; s = LT[j + 2] + LT[j + 3];`, ops: { rd: 4, add: 3 }, bytes: 8 },
  // The bend's 17 chord ends from 9 sines: the ends sit at headings pi - j
  // pi/16, so sin(j pi/16) and cos(j pi/16) = sin((8 - j) pi/16) (the swap),
  // mirrored past j = 8 (sin(pi - x) = sin x, cos(pi - x) = -cos x); the pose
  // at w 0 is the bend's centre plus 109 m along the normal (pose()). The two
  // straight sentinels (CH's +-1e4) would add 8 numbers (counted in entries).
  ptabSym: { sizes: [17], table: 1, entries: 17, build: () => `const LT = []; for (let k = 0; k <= 8; ++k) LT.push(M.sin(k * PI / 16));`,
    look: () => `const j = a | 0, b = j > 8, sj = LT[b ? 16 - j : j], C = b ? LT[j - 8] : -LT[8 - j]; c = 650 - 109 * sj + (109 * C - 109); s = C + sj;`,
    ops: { rd: 2, add: 6, mul: 2, cmp: 1 }, bytes: 8 },
  // 19 separate 4-arrays (what caching pose()'s own results would keep).
  ptabObj: { sizes: [19], table: 1, build: () => `const LT = []; for (let i = 0; i < 19; ++i) LT.push([650 + i, -218 + i, .5, .5]);`, look: () => `const e = LT[a | 0]; c = e[0] + e[1]; s = e[2] + e[3];`, ops: { rd: 5, add: 2 }, bytes: 0 },
  // The nearest of N nodes and a second-order Taylor step (small table).
  tay: { sizes: [16, 32, 64], build: N => `const LT = new Float32Array(${2 * N + 2}); for (let i = 0; i <= ${N}; ++i) { LT[2 * i] = M.cos(i * PI / ${N}); LT[2 * i + 1] = M.sin(i * PI / ${N}); }`,
    look: N => `const i = (a * ${N / Math.PI} + .5) | 0, h = a - i * ${Math.PI / N}, g = h * h * .5, j = 2 * i, C = LT[j], S = LT[j + 1]; c = C - h * S - g * C; s = S + h * C - g * S;`,
    ops: { mul: 8, add: 7, rd: 2 }, bytes: 4 },
  // 16-bit Q14 in a string (charCodeAt: a method call a read).
  str: { sizes: [128, 256], build: N => `let LS = ''; for (let i = 0; i <= ${N}; ++i) LS += String.fromCharCode(M.round(16384 * M.cos(i * PI / ${N})) & 65535, M.round(16384 * M.sin(i * PI / ${N})) & 65535); const LT = LS;`,
    look: N => `const t = a * ${N / Math.PI}, i = t | 0, u = t - i, j = 2 * i, c0 = LT.charCodeAt(j) << 16 >> 16, s0 = LT.charCodeAt(j + 1) << 16 >> 16; c = (c0 + ((LT.charCodeAt(j + 2) << 16 >> 16) - c0) * u) * 6.103515625e-5; s = (s0 + ((LT.charCodeAt(j + 3) << 16 >> 16) - s0) * u) * 6.103515625e-5;`,
    ops: { mul: 6, add: 17, rd: 0, call: 4, prop: 4 }, bytes: 2 },
};
const lookOf = (f, N) => typeof f.look === 'function' ? f.look(N) : f.look;
const est = o => (o.trig || 0) * P.trig + (o.mul || 0) * P.mul + (o.add || 0) * P.add + (o.rd || 0) * P.rd + (o.call || 0) * P.call + (o.prop || 0) * P.prop + (o.cmp || 0) * 3.2;

// Errors over the bend's poses in the panning frames (the director and WIDE
// 2 at both of its places), VE's ends at w 11, 22.6 and 40.
function frames() {
  const L = []; for (let x = Math.ceil(OB); x < 650; x += 5) L.push(x);
  return [[-100, 0], [-14, 0], [-14, 8], [-3, 0], [-3, 8]].flatMap(([w2, cam]) => runSet(null, true, w2, L, cam).out);
}
function errorOf(fr, name, N) {
  const f = FORMS[name];
  const cs = new Function('M', 'PI', `${f.build(N)}; return a => { let c, s; ${lookOf(f, N)} return [c, s]; };`)(Math, Math.PI);
  const k = -1 / 120, r = 1 / k + DNR;
  const pos = (g, w, C, S) => [650 + (S - 0) * r - w * S, -218 - (C + 1) * r + w * C];
  let m = 0, px = 0;
  for (const f0 of fr) {
    const pc = f0.g.pc;
    for (const p of f0.poses) {
      if (!(p[1] > OB && p[1] <= 650)) continue;
      const a = Math.PI + k * (p[1] - OB), [c, s] = cs(a);
      for (const w of p[2] === 0 ? [11, 22.6, 40] : [p[2]]) {
        const A = pos(p[1], w, Math.cos(a), Math.sin(a)), B = pos(p[1], w, c, s), d = Math.hypot(A[0] - B[0], A[1] - B[1]);
        const Z = ((A[0] - pc[0]) * pc[2] + (A[1] - pc[1]) * pc[3]) / pc[4];
        m = Math.max(m, d); if (Z > .02) px = Math.max(px, d / Z);
      }
    }
  }
  return { m, px };
}

// The m32 QuickJS: the app evaluated with the form appended to derby_view.js
// (its table and __lk kept), then a timing loop of __lk against the trig one.
const BIN = path.join(ROOT, '.cache/derby_host/m32/test-derby'), LOWERED = path.join(ROOT, '.cache/trigcull/code/low/apps/derby'); // code_bytes.mjs's lowered 44fce25
function guest(name, N, time) {
  const f = FORMS[name], dir = path.join(ROOT, '.cache/trigcull/lut', `${name}${N}`, 'apps/derby');
  fs.rmSync(dir, { recursive: true, force: true }); fs.mkdirSync(dir, { recursive: true });
  for (const x of fs.readdirSync(LOWERED)) fs.copyFileSync(path.join(LOWERED, x), path.join(dir, x));
  const lk = `function __lk(a) { let c, s; ${lookOf(f, N)} return c + s; }`;
  let src = `\n(function () { ${f.build(N)}\n globalThis.__LT = ${f.build(N) ? 'LT' : '0'}; })();\nconst LT = globalThis.__LT;\n${lk}\n`;
  if (time) src += `{ const n = 20000; let z = 0, t0 = Date.now(); for (let r = 0; r < 20; ++r) for (let i = 0; i < n; ++i) z += __lk(i * 3.1e-5 * 5);
    const t1 = Date.now(); for (let r = 0; r < 20; ++r) for (let i = 0; i < n; ++i) z += M.cos(i * 1.55e-4) + M.sin(i * 1.55e-4);
    const t2 = Date.now(); for (let r = 0; r < 20; ++r) for (let i = 0; i < n; ++i) z += i * 1.55e-4; const t3 = Date.now();
    console.log('LUTTIME ' + (t1 - t0) + ' ' + (t2 - t1) + ' ' + (t3 - t2) + ' ' + (z > 0)); }\n`;
  fs.appendFileSync(path.join(dir, 'derby_view.js'), src);
  const p = spawnSync(BIN, [], { cwd: ROOT, encoding: 'utf8', env: { ...process.env, DERBY_APP_DIR: dir, DERBY_EVAL_ONLY: '1' } });
  const m = /after eval (\d+)/.exec(p.stdout), t = /LUTTIME (\d+) (\d+) (\d+)/.exec(p.stdout);
  if (!m) throw new Error(`${name}${N}: ${p.stdout.slice(-800)} ${p.stderr.slice(-800)}`);
  return { after: +m[1], time: t ? [+t[1], +t[2], +t[3]] : null };
}

// --bench: writes bench/lut.js, the device's unit prices of these look-ups
// (the pancost bench's protocol: run it from the DERBY WATCH row of a
// DERBY_BGCOST_SOURCE image, summarise with pancost_device.py; each case's
// slope over n is the cost of one look-up, the empty loop subtracted).
if (process.argv.includes('--bench')) {
  const pick = [['trig', 0], ['arr', 128], ['f64', 128], ['f32', 128], ['i16', 128], ['sin1', 128], ['qw', 128], ['tay', 32], ['str', 128],
    ['ptabArr', 19], ['ptab64', 19], ['ptab32', 19], ['ptabObj', 19]];
  const tables = pick.map(([n, N]) => `  const T_${n} = (function () { ${FORMS[n].build(N)}\n    return ${FORMS[n].build(N) ? 'LT' : '0'}; })();`).join('\n');
  const cases = pick.map(([n, N]) => `    ${n}: k => { const LT = T_${n}; let z = 0, c = 0, s = 0; for (let i = 0; i < k; ++i) { const a = (i & 1023) * .00306; ${lookOf(FORMS[n], N)} z += c + s; } sink = z; },`).join('\n');
  const src = `// LUT look-up bench (docs/apps/derby-trig-cull.md section 5), generated by
// tools/games/ovalcost/lut_forms.mjs --bench: do not edit. Same protocol as
// tools/games/pancost/bench/derby_watch.js (BGS lines, MDT js= turns; the
// slope over n per case is one look-up plus the loop's angle and sum).
(function () {
  'use strict';
  const M = Math, PI = M.PI;
${tables}
  const P0 = [[650 + 24, -218, -1, 0], [120 * PI, -1 / 120, 650, -218, -1, 0, PI], [2e3, 0, 650, 0, 1, 0]];
  // pose() on the bend as the app has it (derby_view.js), for the reference.
  function pose(g, w) {
    let i = 0;
    while (i < 2 && g > P0[i][0]) g -= P0[i++][0];
    const e = P0[i];
    let x = e[2], z = e[3], c = e[4], d = e[5];
    if (e[1]) { const r = 1 / e[1] + 11, C = M.cos(e[6] + e[1] * g), S = M.sin(e[6] + e[1] * g); x += (S - d) * r; z -= (C - c) * r; c = C; d = S; g = 0; }
    return [x + g * c - w * d, z + g * d + w * c, c, d];
  }
  let sink = 0;
  const JS = {
    loop: k => { let z = 0; for (let i = 0; i < k; ++i) { const a = (i & 1023) * .00306; z += a; } sink = z; },
${cases}
    pose: k => { let z = 0; for (let i = 0; i < k; ++i) { const p = pose(700 + (i & 255), 22.6); z += p[0]; } sink = z; }
  };
  const CASES = [];
  for (const k in JS) for (const n of [0, 200, 400]) CASES.push([k, n]);
  const HOLD = 40, V = pocket.kasane, H = V.procedural, log = m => console.log('BGS ' + m);
  H.beginFrame(0);
  const res = H.resource();
  let t = 0, built = 0;
  globalThis.frame = function () {
    const c = CASES[(t / HOLD | 0) % CASES.length];
    if (t % HOLD === 0) log(c[0] + ' ' + c[1]);
    ++t;
    H.beginFrame(0);
    try { JS[c[0]](c[1]); } catch (e) { if (t % HOLD === 1) log('ERR ' + c[0] + ' ' + e); }
    try { H.commit(); } catch (e) {}
    if (!built) { built = 1; V.replace(tx => { tx.background(255); tx.image({resource: res, bounds: [0, 0, 240, 135], sourceWidth: 240, sourceHeight: 135}); }); }
    else V.patch(() => {});
  };
  log('READY cases=' + CASES.length + ' sink=' + sink);
})();
`;
  const out = path.join(ROOT, 'tools/games/ovalcost/bench/lut.js');
  fs.mkdirSync(path.dirname(out), { recursive: true });
  fs.writeFileSync(out, src);
  // The bench must run: every case once in Node (same JS).
  const g = { pocket: { kasane: { procedural: { beginFrame() {}, resource() { return 0; }, commit() {} }, replace() {}, patch() {} } }, console: { log() {} } };
  new Function('pocket', 'console', 'globalThis', src)(g.pocket, g.console, g);
  for (let i = 0; i < 40 * 3 * 15; ++i) g.frame();
  console.log('wrote', path.relative(ROOT, out));
  process.exit(0);
}

const fr = frames();
const rows = [];
for (const [name, f] of Object.entries(FORMS)) for (const N of f.sizes) {
  const e = name === 'trig' || f.table ? { m: name === 'ptab32' ? 5e-4 : 0, px: NaN } : errorOf(fr, name, N);
  const row = { name, N, entries: name === 'trig' || name === 'poly' ? 0 : f.table ? (f.entries || 76) : name === 'qt2' || name === 'qt3' ? N / 2 + 1 : name === 'sin1' ? 3 * N / 2 + 2 : name === 'qw' ? N / 2 + 2 : 2 * N + 2, ...e, us: est(f.ops) };
  if (GUEST) {
    const g = guest(name, N, false), t = guest(name, N, true);
    row.after = g.after; row.time = t.time;
  }
  rows.push(row);
}
const base = rows.find(r => r.name === 'trig');
console.log(`| 形 | N | 要素 | データ B（計算） | ゲストの増分 B（m32 実測、Math 版との差） | 最大誤差 m | 最大誤差 px | 1 回の推定 µs（実機の単価） | host QuickJS の時間（Math.cos+sin = 1） |`);
console.log('| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |');
for (const r of rows) {
  const f = FORMS[r.name], data = r.entries * (f.bytes || 0);
  const g = GUEST ? (r.after - base.after) : '—';
  const t = GUEST && r.time ? ((r.time[0] - r.time[2]) / (base.time[0] - base.time[2])).toFixed(2) : '—';
  console.log(`| ${r.name} | ${r.N || '—'} | ${r.entries} | ${data} | ${g} | ${r.m.toExponential(1)} | ${r.px.toExponential(1)} | ${r.us.toFixed(1)} | ${t} |`);
}
