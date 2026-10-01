// Number-type bench (docs/apps/derby-pan-camera-cost.md "整数と倍精度"): runs
// from the DERBY WATCH row in a DERBY_BGCOST_SOURCE image. Each case [name,
// n] holds for HOLD frames and is announced by one 'BGS name n' line; the per
// iteration cost is the slope of the JS turn (MDT js=) over n, less the empty
// loop's (tools/games/pancost/pancost_device.py). QuickJS keeps small
// integers as tagged int32 and everything else as IEEE doubles; the S3's FPU
// is single precision only.
(function () {
  'use strict';
  const M = Math, imul = M.imul;
  let sink = 0;
  const A64 = [], I32 = new Int32Array(64), I16 = new Int16Array(64), F32 = new Float32Array(64), F64 = new Float64Array(64);
  for (let i = 0; i < 64; ++i) { A64.push(i); I32[i] = i; I16[i] = i; F32[i] = i + .5; F64[i] = i + .5; }
  // Loop bodies. i* keep int32 (results stay small or are truncated), d*
  // are doubles, q* fixed point (Q8, Q16) in int32.
  const JS = {
    loop: n => { for (let i = 0; i < n; ++i) {} },
    iadd: n => { let s = 1; for (let i = 0; i < n; ++i) s = (s + 7) & 1023; sink = s; },
    iadd0: n => { let s = 1; for (let i = 0; i < n; ++i) s = s + 7; sink = s; },
    imul: n => { let s = 1; for (let i = 0; i < n; ++i) s = (s * 3) & 1023; sink = s; },
    iimul: n => { let s = 1; for (let i = 0; i < n; ++i) s = imul(s, 7) | 1; sink = s; },
    icmp: n => { let s = 0; const b = 500; for (let i = 0; i < n; ++i) if (i < b) s = 1; sink = s; },
    ishl: n => { let s = 1; for (let i = 0; i < n; ++i) s = (s << 1) & 65535 | 1; sink = s; },
    ishr: n => { let s = 65535; for (let i = 0; i < n; ++i) s = (s >> 1) | 32768; sink = s; },
    q8: n => { let s = 300; const b = 257; for (let i = 0; i < n; ++i) s = (s * b) >> 8; sink = s; },
    q16: n => { let s = 30000; const b = 32767; for (let i = 0; i < n; ++i) s = (s * b) >> 15; sink = s; },
    q16i: n => { let s = 30000; const b = 32767; for (let i = 0; i < n; ++i) s = imul(s, b) >> 15; sink = s; },
    dadd: n => { let s = 1.5; const b = .37; for (let i = 0; i < n; ++i) s = s + b; sink = s; },
    dmul: n => { let s = 1.5; const b = 1.0000001; for (let i = 0; i < n; ++i) s = s * b; sink = s; },
    ddiv: n => { let s = 1.5; const b = 1.0000001; for (let i = 0; i < n; ++i) s = s / b; sink = s; },
    dcmp: n => { let s = 0; const a = 1.5, b = 2.5; for (let i = 0; i < n; ++i) if (a < b) s = 1; sink = s; },
    mix: n => { let s = 1.5; for (let i = 0; i < n; ++i) s = s + 7; sink = s; },
    mixm: n => { let s = 0; const b = .5; for (let i = 0; i < n; ++i) s = i * b; sink = s; },
    ar: n => { let s = 0; for (let i = 0; i < n; ++i) s = A64[i & 63]; sink = s; },
    aw: n => { for (let i = 0; i < n; ++i) A64[i & 63] = 7; },
    i32r: n => { let s = 0; for (let i = 0; i < n; ++i) s = I32[i & 63]; sink = s; },
    i32w: n => { for (let i = 0; i < n; ++i) I32[i & 63] = 7; },
    i16r: n => { let s = 0; for (let i = 0; i < n; ++i) s = I16[i & 63]; sink = s; },
    i16w: n => { for (let i = 0; i < n; ++i) I16[i & 63] = 7; },
    f32r: n => { let s = 0; for (let i = 0; i < n; ++i) s = F32[i & 63]; sink = s; },
    f32w: n => { const x = 2.37; for (let i = 0; i < n; ++i) F32[i & 63] = x; },
    f64r: n => { let s = 0; for (let i = 0; i < n; ++i) s = F64[i & 63]; sink = s; },
    f64w: n => { const x = 2.37; for (let i = 0; i < n; ++i) F64[i & 63] = x; }
  };
  const CASES = [];
  for (const k in JS) for (const n of [0, 500, 1000]) CASES.push([k, n]);
  const HOLD = 40, V = pocket.kasane, H = V.procedural, log = m => console.log('BGS ' + m);
  H.beginFrame(0);
  const res = H.resource(), dot = H.register([[7, 0, 0, 1, 0, 0], [9, 0, 0, 1, 0, 65535]]);
  let t = 0, built = 0;
  globalThis.frame = function () {
    const c = CASES[(t / HOLD | 0) % CASES.length];
    if (t % HOLD === 0) log(c[0] + ' ' + c[1]);
    ++t;
    H.beginFrame(0);
    H.draw(dot, [120, 130]);
    JS[c[0]](c[1]);
    H.commit();
    if (!built) {
      built = 1;
      V.replace(tx => { tx.background(255); tx.image({resource: res, bounds: [0, 0, 240, 135], sourceWidth: 240, sourceHeight: 135}); });
    } else V.patch(() => {});
  };
  log('READY cases=' + CASES.length);
})();
