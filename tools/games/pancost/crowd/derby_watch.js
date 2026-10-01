// Crowd-as-noise cost bench (docs/apps/derby-pan-camera-cost.md, "観客をノイズ
// にする案"): runs from the DERBY WATCH row in a DERBY_BGCOST_SOURCE image.
// Each case [name, n] holds HOLD frames after one 'BGS name n' line; the
// analyser takes the slope of the JS turn and of the band time over n.
//   vcP   today's crowd plan, P dots a draw, n draws a frame
//   vbP   the same with the blink input flipped every frame
//   ttP   a typed tile of P points registered once, drawn n times (what a
//         per-draw translation would cost: coefficients are fixed at register)
//   trP   n typed tiles of P points re-registered every frame (today's only
//         way to move one), the program parsed once
//   band  one draw of n full-width horizontal lines (the coarse crowd)
(function () {
  'use strict';
  const OPS = 'SIAMNREVPLQBplC', FLD = ['14', '12', '123', '123', '12', '2', '', '23', '235', '235', '2', '23', '123', '123', '25'];
  function prog(src, arg) {
    const c = [], w = src.split(' ');
    for (let i = 0; i < w.length; ++i) {
      const t = w[i], o = OPS.indexOf(t[0]), f = FLD[o], v = t.slice(1).split(','), row = [o, 0, 0, 0, 0, 0];
      for (let j = 0; j < f.length; ++j) row[+f[j]] = v[j][0] === '$' ? arg[+v[j].slice(1)] : +v[j];
      c.push(row);
    }
    return c;
  }
  // apps/derby/derby_prog.js T.crowd, verbatim.
  const CROWD = 'I8,0 I9,1 I10,2 I11,3 I12,4 I14,6 S15,12650 M14,14,15 S15,46496 A14,14,15 S15,105642 S1,1.5 S2,2.39996 S4,-1 S3,$6 M3,3,9 S13,0 S7,.5 M5,11,7 A5,5,10 R$1 I0,0 I6,5 A6,6,13 Q12 R$2 N7,6 M7,7,1 A7,7,0 p14,7,5 A0,0,3 A6,6,2 M14,14,4 A14,14,15 E E A5,5,11 S7,.9 A13,13,7 E';
  // 4 rows x 8 dots = 32 dots a bay; input 4 (bays) makes 32/64/128.
  const KC = [5, 4, 8, 8, 4, 5, 1 / 8];
  const BAND = 'I0,0 I1,1 I2,2 I3,3 S4,1 Q2 V0,1 L3,1,33808 A1,1,4 E';
  const V = pocket.kasane, H = V.procedural, log = m => console.log('BGS ' + m), SIZES = [32, 64, 128];
  H.beginFrame(0);
  const res = H.resource(), NOP = prog('S0,0'), P = {crowd: H.register(prog(CROWD, KC)), band: H.register(prog(BAND))};
  // A tile: a random walk of p points in a 28 x 12 px bay, from a hash
  // (a stand-in for "noise"; the look is not judged here).
  function tile(p, seed) {
    const x = [], y = [];
    let h = seed * 2654435761 >>> 0, u = 14, v = 6;
    for (let i = 0; i < p; ++i) {
      h = (h ^ h << 13) >>> 0; h = (h ^ h >>> 17) >>> 0; h = (h ^ h << 5) >>> 0;
      u = Math.max(0, Math.min(27, u + (h & 7) - 3)); v = Math.max(0, Math.min(11, v + (h >>> 3 & 3) - 1.5 | 0));
      x.push(u); y.push(v);
    }
    return {x: x, y: y};
  }
  const TILES = {};
  for (const p of SIZES) TILES[p] = [tile(p, 1), tile(p, 2), tile(p, 3)];
  const desc = (tl, X, Y, col) => ({kind: 'affineQ14Points', x: tl.x, y: tl.y, color: col, coeff: [16384, 0, 0, 16384, X * 16384, Y * 16384]});
  const CASES = [];
  // A frame holds 1,024 segments: n stops at 24/12/6 draws for 32/64/128.
  const DN = {32: [0, 8, 16, 24], 64: [0, 4, 8, 12], 128: [0, 2, 4, 6]};
  for (const p of SIZES) for (const n of DN[p]) CASES.push(['vc' + p, n]);
  for (const n of DN[64]) CASES.push(['vb64', n]);
  for (const p of SIZES) for (const n of DN[p]) CASES.push(['tt' + p, n]);
  for (const p of SIZES) for (const n of [0, 1, 2, 4]) CASES.push(['tr' + p, n]);
  for (const n of [0, 6, 12, 24]) CASES.push(['band', n]);
  const HOLD = 40;
  let t = 0, built = 0, TT = null, prev = [], f0 = 0, mf = 0;
  globalThis.frame = function () {
    // info() samples the heap at the start of a turn (null while
    // evaluating): register in one frame, read the difference in later ones.
    if (mf < 3) {
      const m = pocket.memory.info().internalFreeBytes;
      if (mf === 0) f0 = m;
      else if (mf === 1) {
        TT = {};
        for (const p of SIZES) TT[p] = TILES[p].map((tl, i) => H.register(NOP, desc(tl, 20 + 70 * i, 40, 50712)));
      } else log('MEM tiles=9 32/64/128x3 before=' + f0 + ' after=' + m);
      ++mf;
    }
    const c = CASES[(t / HOLD | 0) % CASES.length], k = c[0], n = c[1];
    if (t % HOLD === 0) log(k + ' ' + n);
    ++t;
    for (const h of prev) H.unregister(h);
    prev = [];
    H.beginFrame(0);
    H.draw(P.band, [120, 130, 1, 121]);
    try {
      if (k[0] === 'v') {
        const bays = +k.slice(2) / 32, blink = k[1] === 'b' ? t & 1 : 0;
        for (let j = 0; j < n; ++j) H.draw(P.crowd, [4 + (j % 8) * 29, 30 + (j >> 3) * 25, 28, -2.4, bays, .3 + j, blink]);
      } else if (k[0] === 't' && k[1] === 't') {
        const hs = TT[+k.slice(2)];
        for (let j = 0; j < n; ++j) H.draw(hs[j % 3], []);
      } else if (k[0] === 't') {
        const ts = TILES[+k.slice(2)];
        for (let j = 0; j < n; ++j) {
          const h = H.register(NOP, desc(ts[j % 3], 10 + 50 * j, 90, 50712));
          prev.push(h);
          H.draw(h, []);
        }
      } else if (n) H.draw(P.band, [0, 20, n, 239]);
    } catch (e) { if (t % HOLD === 1) log('ERR ' + k + ' ' + e); }
    try { H.commit(); } catch (e) {}
    if (!built) {
      built = 1;
      V.replace(tx => { tx.background(255); tx.image({resource: res, bounds: [0, 0, 240, 135], sourceWidth: 240, sourceHeight: 135}); });
    } else V.patch(() => {});
  };
  log('READY cases=' + CASES.length);
})();
