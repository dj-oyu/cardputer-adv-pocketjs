// The JS micro cases of tools/games/pancost/bench/derby_watch.js, copied
// verbatim, each run at n = 0 and n = 1000 under the profiler: the
// difference / 1000 is the bytecode of one loop turn, which the device's
// per-turn microseconds (docs/apps/derby-pan-camera-cost.md section 1) were
// measured on. analyze.py fits the device's price per opcode to them.
(function () {
'use strict';
// Inside a function, as on the device (its names are closure variables).
const M = Math, sqrt = M.sqrt, atan2 = M.atan2, sin = M.sin, cos = M.cos;
const WX = [], WZ = [], cy = cos(.5), sy = sin(.5), cx = 0, cz = 0, f = 100, hy = 33, h = 9.7, NEAR = 4;
for (let i = 0, x = -20; i < 256; ++i, x += 1.3 + .7 * sin(i)) { WX.push(x); WZ.push(11); }
const OUT = [], OUTF = new Float32Array(512), A64 = [], F64 = new Float32Array(64), I64 = new Int16Array(64);
for (let i = 0; i < 512; ++i) OUT.push(0);
for (let i = 0; i < 64; ++i) A64.push(.5);
const o = {p: 1.5, q: 2.5}, id = v => v;
function proj(n, out) {
  let j = 0;
  for (let i = 0; i < n; ++i) {
    const dx = WX[i] - cx, dz = WZ[i] - cz, z = dx * sy + dz * cy;
    if (z < NEAR) continue;
    const k = f / z;
    out[j++] = 120 + (dx * cy - dz * sy) * k;
    out[j++] = hy + h * k;
  }
  return j;
}
let sink = 0;
const JS = {
  loop: n => { for (let i = 0; i < n; ++i) {} },
  mul: n => { let s = 1.5; const b = 1.0000001; for (let i = 0; i < n; ++i) s = s * b; sink = s; },
  add: n => { let s = 1.5; const b = .37; for (let i = 0; i < n; ++i) s = s + b; sink = s; },
  div: n => { let s = 1.5; const b = 1.0000001; for (let i = 0; i < n; ++i) s = s / b; sink = s; },
  sqrt: n => { let s = 0; const x = 2.37; for (let i = 0; i < n; ++i) s = sqrt(x); sink = s; },
  atan2: n => { let s = 0; const x = 2.37, y = .83; for (let i = 0; i < n; ++i) s = atan2(y, x); sink = s; },
  sin: n => { let s = 0; const x = 1.37; for (let i = 0; i < n; ++i) s = sin(x); sink = s; },
  cos: n => { let s = 0; const x = 1.37; for (let i = 0; i < n; ++i) s = cos(x); sink = s; },
  msin: n => { let s = 0; const x = 1.37; for (let i = 0; i < n; ++i) s = M.sin(x); sink = s; },
  arr: n => { const x = 2.37; for (let i = 0; i < n; ++i) A64[i & 63] = x; },
  arrr: n => { let s = 0; for (let i = 0; i < n; ++i) s = A64[i & 63]; sink = s; },
  f32: n => { const x = 2.37; for (let i = 0; i < n; ++i) F64[i & 63] = x; },
  i16: n => { const x = 2.37; for (let i = 0; i < n; ++i) I64[i & 63] = x; },
  call: n => { let s = 0; const x = 2.37; for (let i = 0; i < n; ++i) s = id(x); sink = s; },
  prop: n => { let s = 0; for (let i = 0; i < n; ++i) s = o.p; sink = s; },
  lit8: n => { let s = null; const x = 2.37; for (let i = 0; i < n; ++i) s = [x, x, x, x, x, x, x, x]; sink = s; },
  projp: n => { sink = proj(n, OUT); }
};
for (const k in JS) for (const n of [0, 1000]) {
  // projp has 256 points; its turns are measured per point at 256.
  const m = k === 'projp' ? (n ? 256 : 0) : n;
  __opprof(1); JS[k](m); __opprof(0, k + ' ' + m);
}
})();
