// Background-cost synthetic load (docs/kasane/derby-background-cost.md): runs
// from the DERBY WATCH row in a DERBY_BGCOST_SOURCE image. Each case is held
// for HOLD frames and announced by one 'BGS' line; the MDT/BGC lines that
// follow it belong to it. The program text form is DERBY WATCH's.
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
  // seg: n segments from (x, y) stepping (dx, dy), each (lx, ly) long.
  const SRC = {
    seg: 'I0,0 I1,1 I2,2 I3,3 I4,4 I5,5 I6,6 Q4 V0,1 A8,0,5 A9,1,6 L8,9,65535 A0,0,2 A1,1,3 E',
    alu: 'I0,0 I1,1 I4,4 Q4 A0,0,1 M1,1,1 E',
    plot: 'I0,0 I1,1 I2,2 I4,4 Q4 P0,1,65535 A0,0,2 E',
    line: 'I0,0 I1,1 I2,2 I4,4 V0,1 Q4 L0,1,65535 A0,0,2 E',
    // SIN of a fixed argument: newlib's sinf reduces large ones the slow way.
    sin: 'I0,0 I4,4 Q4 N1,0 E'
  };
  // Geometry cases [name, y0, y step per draw, lx, ly, y step per segment]:
  // 'off' lies above the panel (scanned by every band, walked by none); v8
  // stays inside one band.
  const GEO = [['off', -50, 0, 0, 0, 0], ['dot', 4, 13, 0, 0, .1], ['h16', 4, 13, 16, 0, .1], ['h60', 4, 13, 60, 0, .1],
    ['v8', 8, 8, 0, 7, 0], ['v64', 4, 7, 0, 63, .1], ['d60', 4, 7, 60, 60, .1]];
  const CASES = [];
  for (const g of GEO) for (let n = 0; n <= 1000; n += 100) CASES.push(['g', g, n]);
  for (const k of ['alu', 'plot', 'line']) for (const d of [0, 1, 2, 4, 8]) CASES.push(['v', k, d]);
  for (const a of [1, 30, 100, 300, 1000, 3000, 30000]) CASES.push(['s', 'sin', a]);
  const HOLD = 40, V = pocket.kasane, H = V.procedural, log = m => console.log('BGS ' + m);
  H.beginFrame(0);
  const res = H.resource(), P = {};
  for (const k in SRC) P[k] = H.register(prog(SRC[k]));
  let t = 0, built = 0;
  globalThis.frame = function () {
    const c = CASES[(t / HOLD | 0) % CASES.length];
    if (t % HOLD === 0) log(c[0] === 'g' ? c[1][0] + ' ' + c[2] : c[1] + ' ' + c[2]);
    ++t;
    H.beginFrame(0);
    if (c[0] === 'g') {
      const g = c[1];
      // One dot on the bottom row makes the damage span every band, so the
      // off-panel segments are scanned by all 17 (without it none is read).
      if (g[1] < 0 && c[2]) H.draw(P.seg, [120, 130, 0, 0, 1, 0, 0]);
      for (let j = 0; j * 100 < c[2]; ++j)
        H.draw(P.seg, [0, g[1] + g[2] * j, 1.8, g[5], mn(100, c[2] - 100 * j), g[3], g[4]]);
    } else if (c[0] === 's') {
      for (let j = 0; j < 4; ++j) H.draw(P.sin, [c[2], 0, 0, 0, 255]);
    } else if (c[1] === 'alu') {
      for (let j = 0; j < c[2]; ++j) H.draw(P.alu, [0, 1, 0, 0, 255]);
    } else {
      for (let j = 0; j < c[2]; ++j) H.draw(P[c[1]], [0, -50, 2, 0, 100]);
    }
    H.commit();
    if (!built) {
      built = 1;
      V.replace(tx => { tx.background(255); tx.image({resource: res, bounds: [0, 0, 240, 135], sourceWidth: 240, sourceHeight: 135}); });
    } else V.patch(() => {});
  };
  const mn = Math.min;
  log('READY cases=' + CASES.length);
})();
