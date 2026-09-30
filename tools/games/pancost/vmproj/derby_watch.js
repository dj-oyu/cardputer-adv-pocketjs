// VM projection bench (docs/apps/derby-pan-camera-cost.md §2): the per-unit
// cost of projecting inside the VM with Newton reciprocals, for rail posts,
// turf stripes (two points a stripe) and crowd bays (a bay's scale, then its
// dots). Runs from the DERBY WATCH row in a DERBY_BGCOST_SOURCE image; each
// case [name, n] holds HOLD frames after one 'BGS name n' line.
//   post    n posts in one draw (the study's first bench, again)
//   stripe  n stripes in one draw: near and far rail points, 2 Newton steps each
//   bay1    n crowd bays of 1 dot in one draw (the bay's own overhead)
//   bay32   n crowd bays of 4 rows x 8 dots in one draw
// Camera-space x is carried times f, so a screen x is 120 + X*r.
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
  // Newton on r (1/z) with t scratch and m = -1: r <- r + r(1 - z r).
  const nt = (t, z, r, m) => `M${t},${z},${r} M${t},${t},${m} M${t},${t},${r} A${t},${t},${r} A${r},${t},${r}`;
  const HY = 33, HF = 970;
  const SRC = {
    post: 'I0,0 I1,1 I2,2 I3,3 I4,4 I5,5 I6,6 I7,7 S9,-1 S10,2 S14,-110 S15,120 Q4 ' +
      'M8,2,5 M8,8,9 A8,8,10 M5,5,8 M8,2,5 M8,8,9 A8,8,10 M5,5,8 ' +
      'M11,0,5 A11,11,15 M12,7,5 A12,12,6 M13,14,5 A13,13,12 V11,12 L11,13,65535 A0,0,1 A2,2,3 E',
    // Inputs: near x, near z, far x, far z, dx, dz, count, 1/near z, with
    // x and z divided by h*f so that a screen y is HY + r (no constant for
    // h*f: all 16 registers are taken). The far seed comes from the near one
    // by 3 Newton steps (depth ratio < 2); each stripe then takes 2 per point.
    stripe: 'I0,0 I1,1 I2,2 I3,3 I4,4 I5,5 I6,6 I7,7 S14,-1 S15,' + HY + ' S8,0 A8,8,7 ' +
      [0, 1, 2].map(() => nt(9, 3, 8, 14)).join(' ') + ' Q6 S6,120 ' +
      nt(9, 1, 7, 14) + ' ' + nt(9, 1, 7, 14) + ' ' + nt(9, 3, 8, 14) + ' ' + nt(9, 3, 8, 14) +
      ' M10,0,7 A10,10,6 A11,7,15 M12,2,8 A12,12,6 A13,8,15' +
      ' V10,11 L12,13,65535 A0,0,4 A1,1,5 A2,2,4 A3,3,5 E',
    // Inputs: x, z, dx, dz (one bay each), bays, 1/z, phase, blink. Bays
    // outer (one reciprocal each), rows and dots inner as in T.crowd.
    bay: 'I0,0 I2,1 I1,2 I3,3 I4,4 I5,5 I6,6 I11,7 S7,12650 M11,11,7 S7,46496 A11,11,7 S13,1.5 S14,2.39996 S15,105642 ' +
      'Q4 S4,-1 ' + nt(7, 2, 5, 4) + ' ' + nt(7, 2, 5, 4) + ' M12,0,5 S7,120 A12,12,7 S7,$2 M10,7,5 S7,' + HF +
      ' M9,7,5 S7,' + HY + ' A9,9,7 R$0 S6,.3 S8,0 A8,8,12 R$1 N7,6 M7,7,13 A7,7,8 p11,7,9 A8,8,10 A6,6,14 M11,11,4 A11,11,15 E' +
      ' S7,-50 M7,7,5 A9,9,7 E A0,0,1 A2,2,3 E'
  };
  const V = pocket.kasane, H = V.procedural, log = m => console.log('BGS ' + m);
  H.beginFrame(0);
  const res = H.resource(), P = {post: H.register(prog(SRC.post)), stripe: H.register(prog(SRC.stripe)),
    bay1: H.register(prog(SRC.bay, [1, 1, 150])), bay32: H.register(prog(SRC.bay, [4, 8, 150])),
    dot: H.register(prog('I0,0 I1,1 P0,1,65535'))};
  const CASES = [];
  for (const k of ['post', 'stripe']) for (const n of [0, 64, 128, 240]) CASES.push([k, n]);
  for (const n of [0, 32, 64, 128]) CASES.push(['bay1', n]);
  for (const n of [0, 4, 8, 16]) CASES.push(['bay32', n]);
  const HOLD = 40;
  let t = 0, built = 0;
  globalThis.frame = function () {
    const c = CASES[(t / HOLD | 0) % CASES.length], k = c[0], n = c[1];
    if (t % HOLD === 0) log(k + ' ' + n);
    ++t;
    H.beginFrame(0);
    H.draw(P.dot, [120, 130]);
    try {
      if (n && k === 'post') H.draw(P.post, [-3000, 30, 20, .3, n, 1 / 20, HY, HF]);
      else if (n && k === 'stripe') H.draw(P.stripe, [-3000 / HF, 20 / HF, -3500 / HF, 31.6 / HF, 30 / HF, .3 / HF, n, HF / 20]);
      else if (n) H.draw(P[k], [-3000, 20, k === 'bay1' ? 45 : 360, k === 'bay1' ? .45 : 3.6, n, 1 / 20, .3, t & 1]);
    } catch (e) { if (t % HOLD === 1) log('ERR ' + k + ' ' + e); }
    try { H.commit(); } catch (e) {}
    if (!built) {
      built = 1;
      V.replace(tx => { tx.background(255); tx.image({resource: res, bounds: [0, 0, 240, 135], sourceWidth: 240, sourceHeight: 135}); });
    } else V.patch(() => {});
  };
  log('READY cases=' + CASES.length + ' instr=' + [SRC.post, SRC.stripe, SRC.bay].map(s => s.split(' ').length).join('/'));
})();
