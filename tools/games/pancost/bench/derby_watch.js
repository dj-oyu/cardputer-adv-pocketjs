// Pan-camera cost bench (docs/apps/derby-pan-camera-cost.md): runs from the
// DERBY WATCH row in a DERBY_BGCOST_SOURCE image. Each case [name, n] holds
// for HOLD frames and is announced by one 'BGS name n' line; the MDT lines
// after it are its turns. The per-unit cost is the slope of the JS turn over
// n (tools/games/pancost/pancost_device.py).
(function () {
  'use strict';
  const OPS = 'SIAMNREVPLQBplC', FLD = ['14', '12', '123', '123', '12', '2', '', '23', '235', '235', '2', '23', '123', '123', '25'];
  function prog(src) {
    const c = [], w = src.split(' ');
    for (let i = 0; i < w.length; ++i) {
      const t = w[i], o = OPS.indexOf(t[0]), f = FLD[o], v = t.slice(1).split(','), row = [o, 0, 0, 0, 0, 0];
      for (let j = 0; j < f.length; ++j) row[+f[j]] = +v[j];
      c.push(row);
    }
    return c;
  }
  const M = Math, sqrt = M.sqrt, atan2 = M.atan2, sin = M.sin, cos = M.cos;
  const SRC = {
    nop: 'S0,0',
    // Four projected points as inputs, drawn as a 3-segment polyline.
    poly: 'I0,0 I1,1 I2,2 I3,3 I4,4 I5,5 I6,6 I7,7 V0,1 L2,3,65535 L4,5,65535 L6,7,65535',
    // Posts along a straight rail projected in the VM: camera-space x (times
    // f) and depth are affine in the post index, 1/z by two Newton steps
    // from the previous post's reciprocal (input 5 seeds post 0 exactly).
    newton: 'I0,0 I1,1 I2,2 I3,3 I4,4 I5,5 I6,6 I7,7 S9,-1 S10,2 S14,-110 S15,120 Q4 ' +
      'M8,2,5 M8,8,9 A8,8,10 M5,5,8 M8,2,5 M8,8,9 A8,8,10 M5,5,8 ' +
      'M11,0,5 A11,11,15 M12,7,5 A12,12,6 M13,14,5 A13,13,12 V11,12 L11,13,65535 A0,0,1 A2,2,3 E'
  };
  // World points: posts at uneven spacing on a line 11 m from a camera
  // yawed 0.5 rad, so every point needs its own divide.
  const WX = [], WZ = [], cy = cos(.5), sy = sin(.5), cx = 0, cz = 0, f = 100, hy = 33, h = 9.7, NEAR = 4;
  for (let i = 0, x = -20; i < 256; ++i, x += 1.3 + .7 * sin(i)) { WX.push(x); WZ.push(11); }
  const OUT = [], OUTF = new Float32Array(512), A64 = [], F64 = new Float32Array(64), I64 = new Int16Array(64);
  for (let i = 0; i < 512; ++i) OUT.push(0);
  for (let i = 0; i < 64; ++i) A64.push(.5);
  const o = {p: 1.5, q: 2.5}, id = v => v, IN8 = [10, 20, 30, 25, 50, 30, 70, 20];
  // Projects n points into out; returns the number of values written.
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
  let P = null, prev = 0, sink = 0;
  // JS micro cases: the loop body only; 'loop' is the empty loop.
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
    lit8: n => { let s = null; const x = 2.37; for (let i = 0; i < n; ++i) s = [x, x, x, x, x, x, x, x]; sink = s; }
  };
  const CASES = [];
  for (const k in JS) for (const n of [0, 500, 1000]) CASES.push(['j', k, n]);
  for (const k of ['projp', 'projf', 'projd', 'pd0', 'nopd', 'nop0'])
    for (const n of [0, 64, 128, 256]) CASES.push(['p', k, n]);
  // REPEAT_REG runs at most 255 times; typed batches hold at most 128 points.
  for (const n of [0, 64, 128, 240]) CASES.push(['p', 'newton', n]);
  for (const n of [0, 32, 64, 128]) CASES.push(['p', 'treg', n]);
  const HOLD = 40, V = pocket.kasane, H = V.procedural, log = m => console.log('BGS ' + m);
  H.beginFrame(0);
  const res = H.resource();
  P = {};
  for (const k in SRC) P[k] = H.register(prog(SRC[k]));
  function draws(c, n) {
    if (c === 'projp') { sink = proj(n, OUT); return; }
    if (c === 'projf') { sink = proj(n, OUTF); return; }
    if (c === 'projd') {
      // Project, then pass four points (8 inputs) per draw.
      const m = proj(n, OUT);
      for (let j = 0; j + 8 <= m; j += 8)
        H.draw(P.poly, [OUT[j], OUT[j + 1], OUT[j + 2], OUT[j + 3], OUT[j + 4], OUT[j + 5], OUT[j + 6], OUT[j + 7]]);
      return;
    }
    if (c === 'pd0') { for (let j = 0; j < n; j += 4) H.draw(P.poly, IN8); return; }
    if (c === 'nopd') { for (let j = 0; j < n; j += 4) H.draw(P.nop, IN8); return; }
    if (c === 'nop0') { for (let j = 0; j < n; j += 4) H.draw(P.nop, []); return; }
    if (c === 'newton') {
      // n posts in one draw: x*f from -3000 by 30, depth 20 by .3.
      if (n) H.draw(P.newton, [-3000, 30, 20, .3, n, 1 / 20, hy, h * f]);
      return;
    }
    if (c === 'treg') {
      // Typed points hold at most 128: re-register the projected batch
      // every frame (the only way to feed it per-frame points), draw once.
      if (prev) { H.unregister(prev); prev = 0; }
      if (!n) return;
      const m = proj(n, OUT), x = [], y = [];
      for (let j = 0; j < m; j += 2) { x.push(OUT[j] | 0); y.push(OUT[j + 1] | 0); }
      prev = H.register(prog(SRC.nop), {kind: 'affineQ14Points', x: x, y: y, color: 65535, coeff: [16384, 0, 0, 16384, 0, 0]});
      H.draw(prev, []);
    }
  }
  let t = 0, built = 0;
  globalThis.frame = function () {
    const c = CASES[(t / HOLD | 0) % CASES.length];
    if (t % HOLD === 0) log(c[1] + ' ' + c[2]);
    ++t;
    H.beginFrame(0);
    // One dot keeps the damage and the band path the same in every case.
    H.draw(P.poly, [120, 130, 120, 130, 120, 130, 120, 130]);
    try { if (c[0] === 'j') JS[c[1]](c[2]); else draws(c[1], c[2]); } catch (e) { if (t % HOLD === 1) log('ERR ' + c[1] + ' ' + e); }
    // A failed draw ends the frame natively; commit then throws too.
    try { H.commit(); } catch (e) {}
    if (!built) {
      built = 1;
      V.replace(tx => { tx.background(255); tx.image({resource: res, bounds: [0, 0, 240, 135], sourceWidth: 240, sourceHeight: 135}); });
    } else V.patch(() => {});
  };
  const mn = M.min;
  log('READY cases=' + CASES.length + ' check=' + proj(256, OUT));
})();
