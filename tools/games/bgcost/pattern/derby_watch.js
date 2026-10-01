// Pattern-line band cost (docs/kasane/crowd-primitives-design.md, P24 stage
// 1): runs from the DERBY WATCH row of a DERBY_BGCOST_SOURCE image built with
// KASANE_BGCOST_TRACE. Each case is held for HOLD frames and announced by one
// 'BGS name n' line. Names:
//   B<geo> plain lines, the band renderer before the pattern line (beginFrame(1))
//   N<geo> plain lines, the new renderer (beginFrame(0)), a frame with no pattern line
//   E<geo> plain lines, the new renderer, one pattern dot in the frame (ext set)
//   P<f><geo> pattern lines: f = a all bits A, h half the bits A and no B, b half A half B
//   Fs/Fp n  the P24 crowd plan ('fan', MID patterns), n rows, 3 copies a
//            frame; s side view (flat), p panning (ends unlike in depth, 8 chords)
(function () {
  'use strict';
  const S = (r, v) => [0, r, 0, 0, v, 0], I = (r, n) => [1, r, n, 0, 0, 0];
  const A = (d, a, b) => [2, d, a, b, 0, 0], M = (d, a, b) => [3, d, a, b, 0, 0];
  const REP = n => [5, 0, n, 0, 0, 0], REPR = r => [10, 0, r, 0, 0, 0], END = [6, 0, 0, 0, 0, 0];
  const MOVE = (a, b) => [7, 0, a, b, 0, 0], LINE = (a, b, c) => [9, 0, a, b, 0, c];
  const PAT = (base, a, b, n) => [15, base, a, b, n, 0];
  // n segments from (x, y) stepping (dx, dy), each (lx, ly) long: inputs
  // x, y, dx, dy, n, lx, ly, B. r7, r8 the far end, r9..r15 the block.
  const seg = [I(0, 0), I(1, 1), I(2, 2), I(3, 3), I(4, 4), I(5, 5), I(6, 6), REPR(4),
    MOVE(0, 1), A(7, 0, 5), A(8, 1, 6), LINE(7, 8, 65535), A(0, 0, 2), A(1, 1, 3), END];
  const pseg = bits => [I(0, 0), I(1, 1), I(2, 2), I(3, 3), I(4, 4), I(5, 5), I(6, 6), I(11, 7),
    S(9, bits), S(10, 65535), S(12, 0.3), S(13, 31.7), S(14, 0), S(15, 0), REPR(4),
    MOVE(0, 1), A(7, 0, 5), A(8, 1, 6), PAT(9, 7, 8, 24), A(0, 0, 2), A(1, 1, 3), END];
  // crowd_prim.mjs's fanRows, P24 (24-cell patterns), MID seats.
  const RED = 0xc228, BLUE = 0x3a7a, SKIN = 0xf5d3;
  const bits = (starts, w, off) => starts.reduce((b, s) => { for (let i = 0; i < w; i++) b |= 1 << (s + off + i); return b; }, 0) >>> 0;
  const SEATS = [0, 5, 10, 14, 19], HEAD = bits(SEATS, 1, 1), BODY = bits(SEATS, 3, 0);
  const fan = rows => [
    I(0, 0), I(1, 1), I(2, 2), I(3, 3), I(9, 4), I(10, 5), I(7, 6), I(8, 7),
    S(11, -1), S(14, RED + BLUE), S(15, RED), S(4, HEAD), S(6, -1),
    REP(rows),
    S(4, HEAD), S(5, SKIN),
    A(12, 1, 11), A(12, 12, 11), A(13, 3, 11), A(13, 13, 11), MOVE(0, 12), PAT(4, 2, 13, 24),
    S(4, BODY), M(5, 15, 11), M(5, 5, 11),
    MOVE(0, 1), PAT(4, 2, 3, 24),
    A(12, 1, 11), A(13, 3, 11), MOVE(0, 12), PAT(4, 2, 13, 24),
    M(15, 15, 11), A(15, 15, 14),
    S(12, 7), A(7, 7, 12), A(8, 8, 12),
    A(1, 1, 9), A(3, 3, 10),
    END];
  const GEO = [['off', -50, 0, 0, 0, 0], ['dot', 4, 13, 0, 0, .1], ['h16', 4, 13, 16, 0, .1], ['h60', 4, 13, 60, 0, .1],
    ['v64', 4, 7, 0, 63, .1], ['d60', 4, 7, 60, 60, .1]];
  const NS = [0, 200, 400, 600, 800], CASES = [];
  for (const g of GEO) for (const n of NS) { CASES.push(['B', g, n]); CASES.push(['N', g, n]); }
  for (const g of GEO.slice(1)) for (const n of NS) CASES.push(['E', g, n]);
  // A pattern line takes two entries: at most 511 a frame.
  const NP = [0, 100, 200, 300, 400];
  for (const f of 'ahb') for (const g of [GEO[0], GEO[1], GEO[2], GEO[3]]) for (const n of NP) CASES.push(['P' + f, g, n]);
  for (const g of [GEO[4], GEO[5]]) for (const n of NP) CASES.push(['Pb', g, n]);
  for (const v of 'sp') for (const n of [1, 2, 3, 4]) CASES.push(['F' + v, null, n]);
  const HOLD = 30, V = pocket.kasane, H = V.procedural, log = m => console.log('BGS ' + m);
  H.beginFrame(0);
  const res = H.resource(), P = {seg: H.register(seg), pa: H.register(pseg(0xffffff)), ph: H.register(pseg(0x555555))};
  for (const n of [1, 2, 3, 4]) P['f' + n] = H.register(fan(n));
  let t = 0, built = 0;
  globalThis.frame = function () {
    const c = CASES[(t / HOLD | 0) % CASES.length], k = c[0], g = c[1], n = c[2];
    if (t % HOLD === 0) log((k[0] === 'F' ? k : k + g[0]) + ' ' + n);
    ++t;
    H.beginFrame(k === 'B' ? 1 : 0);
    if (k[0] === 'F') {
      for (let j = 0; j < 3; ++j) {
        const o = 30 * j;
        if (k === 'Fs') H.draw(P['f' + n], [0, 30 + o, 239, 30 + o, -6, -6, 3.3, 242.3]);
        else H.draw(P['f' + n], [-40, 40 + o, 200, 20 + o, -9, -4, 0.7, 150.7]);
      }
    } else {
      const plan = k[0] === 'P' ? (k[1] === 'a' ? P.pa : P.ph) : P.seg, b = k === 'Pb' ? 2016 : -1;
      // One dot on the bottom row makes the damage span every band (as synth).
      if (n && (g[1] < 0 || k === 'E')) H.draw(k === 'E' ? P.ph : P.seg, [120, 130, 0, 0, 1, 0, 0, 2016]);
      for (let j = 0; j * 100 < n; ++j)
        H.draw(plan, [0, g[1] + g[2] * j, 1.8, g[5], Math.min(100, n - 100 * j), g[3], g[4], b]);
    }
    H.commit();
    if (!built) {
      built = 1;
      V.replace(tx => { tx.background(255); tx.image({resource: res, bounds: [0, 0, 240, 135], sourceWidth: 240, sourceHeight: 135}); });
    } else V.patch(() => {});
  };
  log('READY cases=' + CASES.length);
})();
