// MEGADEMO. Act I: the news set (3x16 frames, program/inputs below).
// Act II: TWIST, ZENITH, LIMIT; plans load per scene. Numeric opcodes match
// ksn_proc_op. Design: docs/kasane/megademo-limit-scenes.md
(function () {
  'use strict';
  const colors = [
    [0x07ff, 0x3dff, 0xb81f, 0xffff],
    [0xfde0, 0xfb00, 0x07ff, 0xffff],
    [0xf81f, 0x781f, 0x07ff, 0xffff]
  ];
  const backdrops = [0x080c, 0x000d, 0x100b];
  const f32 = Math.fround;

  function checkPhase(phase) {
    if (!Number.isInteger(phase) || phase < 0 || phase > 2)
      throw RangeError('phase must be 0..2');
  }
  function checkLayer(layer) {
    if (!Number.isInteger(layer) || layer < 0 || layer > 4)
      throw RangeError('layer must be 0..4');
  }
  function checkFrame(frame) {
    if (!Number.isInteger(frame) || frame < 0 || frame > 47)
      throw RangeError('frame must be 0..47');
  }

  // Act I layers in the text form decoded by prog() below: a gate, a
  // highway, rising bars, a sine, scanlines and a faceted crystal.
  const ACT1 = ['U2 S2,$0 S3,$1 S4,$2 S5,$3 S6,$4 S7,-7 R$5 V0,2 L1,2,$6 L1,3,$6 L0,3,$6 L0,2,$6' +
    ' A0,0,4 A1,1,5 A2,2,6 A3,3,7 E', 'U2 S2,$0 S3,134 S4,23 S5,4.8 R12 V1,2 L0,3,$1 A0,0,4 A1,1,5 E',
    'S0,0 S1,239 I2,0 S3,1.5 S4,1.47 R10 V0,2 L1,2,$0 A2,2,3 M3,3,4 E',
    'S0,0 S1,4 S2,$0 I3,0 S4,$1 S5,$2 R60 M6,0,2 A6,6,3 N6,6 M7,6,4 A7,7,5 L0,7,$3 A0,0,1 E',
    'I0,0 S1,29 S2,0 S3,239 I4,1 S6,2 R4 V2,0 L3,0,$2 A5,0,6 V2,5 L3,5,$0 A0,0,1 A2,2,4 A3,3,4 E' +
    ' S0,88 S1,152 S2,32 S3,98 S4,120 S5,65 I6,2 I7,3 V4,2 L1,5,$2 L4,3,$2 L0,5,$2 L4,2,$2 V4,2' +
    ' L4,3,$3 V0,5 L1,5,$3 V4,2 A4,4,6 A2,2,7 L4,2,$3 V1,5 A1,1,6 A5,5,7 L1,5,$3 S4,120 V4,3' +
    ' A4,4,6 A3,3,7 L4,3,$3 S5,65 V0,5 A0,0,6 A5,5,7 L0,5,$3 V4,2 L1,5,$1 L4,3,$1 L0,5,$1 L4,2,$1'];
  function program(phase, layer) {
    checkPhase(phase);
    checkLayer(layer);
    const n = colors[phase], a = phase === 1;
    return prog(ACT1[layer], [
      [a ? 42 : 5, a ? 125 : 129, a ? 13 : 12, a ? -13 : -12, a ? 3 : 7, a ? 6 : 8, n[0]],
      [a ? 64 : 68, n[1]], [n[2]],
      [f32(f32(.073) + f32(phase * .015)), a ? 7 : 13, phase === 2 ? 42 : 28, n[3]], n][layer]);
  }

  function inputs(frame, layer) {
    checkFrame(frame);
    checkLayer(layer);
    const phase = Math.floor(frame / 16);
    const beat = frame % 16;
    const pulse = f32(Math.sin(f32(f32(beat) * f32(0.3926990817))));
    const result = [0, 0, 0, 0];
    if (layer === 0) {
      const inset = phase === 1 ? 19 : (phase === 2 ? 3 : 8);
      const sway = f32(pulse * (phase === 2 ? 9 : 3));
      result[0] = f32(inset + sway);
      result[1] = f32(f32(239 - inset) + sway);
    } else if (layer === 1) {
      result[0] = f32(-18 + f32(pulse * 4));
      result[1] = f32(91 + f32(pulse * 3));
    } else if (layer === 2) {
      result[0] = phase === 1 ? f32(70 + pulse) : f32(65 + f32(pulse * 2));
    } else if (layer === 3) {
      result[0] = f32(f32(beat) * f32(0.31));
    } else {
      result[0] = (phase === 2 ? 9 : 17) + ((frame * 7) % 19);
      result[1] = (frame % 5 - 2) * (phase === 2 ? 2 : 1);
      result[2] = f32(f32(pulse * 12) +
        (phase === 2 ? (((frame * 3) % 5) - 2) * 2 : 0));
      result[3] = f32(f32(1 - Math.abs(pulse)) * 8);
    }
    return result;
  }

  // A fixed connected trace, transformed by the native Q14 batch path once
  // per draw. Source points and coefficients are built only at registration.
  function pointBatch(phase, layer) {
    checkPhase(phase);
    checkLayer(layer);
    if (layer !== 3) return null;
    const x = [], y = [];
    for (let i = 0; i < 40; ++i) {
      x.push(18 + i * 5);
      y.push(92 + ((i * 13) % 23) - 11);
    }
    const coeff = [
      [16384, 0, 0, 16384, 0, 0],
      [16000, 1600, -1000, 16384, -6 * 16384, 6 * 16384],
      [15360, -2304, 1300, 15500, 18 * 16384, -10 * 16384]
    ];
    return {kind: 'affineQ14Points', x: x, y: y,
      coeff: coeff[phase].slice(), color: colors[phase][0]};
  }

  // ---- Act II. Apple II hi-res palette; scene cost is set only in KN.
  const WH = 0xffff, VI = 0xfa3f, GR = 0x17a7, BL = 0x167f, OR = 0xfb47;
  // [LIGHT, MID, HEAVY]. c: TWIST corridor (n frames, r ratio, m rungs per
  // frame, s arch segments, kf/ka first frame/arch, D twist); z: ZENITH;
  // l: LIMIT; q: rotate the radar (ZENITH, LIMIT) and the thumbnail, each
  // ~23 ms of band re-renders a frame on the device. Derivation:
  // docs/kasane/megademo-limit-scenes.md, megademo-device-limits.md
  // TWIST and ZENITH hold the display rate at HEAVY's load, so MID shares it.
  const CH = {n: 26, r: .8, m: 3, s: 8, kf: 2, ka: 3, D: 6},
    ZH = {st: 60, gl: 14, ln: 15, cr: 3, cc: 8, sa: 4, ts: 16, rr: 4};
  const KN = [
    {q: [0, 0, 0], c: {n: 12, r: .7, m: 2, s: 4, kf: 2, ka: 3, D: 4}, z: {st: 24, gl: 8, ln: 7, cr: 2, cc: 4, sa: 2, ts: 6, rr: 2},
     l: {n: 12, r: .7, m: 2, s: 4, kf: 2, ka: 3, D: 6, gl: 8, ln: 7, dn: 150, ai: 6, ao: 12, lt: 1.5}},
    {q: [0, 0, 0], c: CH, z: ZH,
     l: {n: 18, r: .76, m: 3, s: 6, kf: 2, ka: 3, D: 7, gl: 11, ln: 11, dn: 200, ai: 12, ao: 22, lt: 2.5}},
    {q: [1, 0, 0], c: CH, z: ZH,
     l: {n: 26, r: .8, m: 3, s: 8, kf: 2, ka: 2, D: 8, gl: 14, ln: 15, dn: 285, ai: 16, ao: 37, lt: 5.7}}
  ];
  // Programs are text, decoded by one function (a generator function per
  // plan costs guest heap at compile time). A letter per ksn_proc_op in
  // opcode order, then its fields; $n is an argument. X x,y,zr,zi,zn,t0,t1,z0
  // is (x,y) *= z in place (zn = -zi, z0 reads zero); U n,o loads inputs.
  const OPS = 'SIAMNREVPLQBplC';
  const FIELDS = ['14', '12', '123', '123', '12', '2', '', '23', '235', '235', '2', '23', '123', '123', '25'];
  function prog(src, arg) {
    const c = [], w = src.split(' ');
    for (let i = 0; i < w.length; ++i) {
      const t = w[i], v = t.slice(1).split(',').map(x => x[0] === '$' ? arg[x[1]] : +x);
      if (t[0] === 'U') {
        for (let j = 0; j < v[0]; ++j) c.push([1, (v[1] || 0) + j, j, 0, 0, 0]);
      } else if (t[0] === 'X') {
        const x = v[0], y = v[1], r = v[2], a = v[5], b = v[6];
        c.push([3, a, x, r, 0, 0], [3, b, y, v[4], 0, 0], [2, a, a, b, 0, 0], [3, b, x, v[3], 0, 0],
          [3, y, y, r, 0, 0], [2, y, y, b, 0, 0], [2, x, a, v[7], 0, 0]);
      } else {
        const o = OPS.indexOf(t[0]), f = FIELDS[o], row = [o, 0, 0, 0, 0, 0];
        for (let j = 0; j < f.length; ++j) row[+f[j]] = v[j];
        row[4] = f32(row[4]);
        c.push(row);
      }
    }
    if (c.length > 64) throw RangeError('64 instructions');
    return c;
  }
  // fr frames (16 registers), ra two polylines, ru rungs, ar CUBIC arches,
  // st stars, gr ground, la lanes, ci skyline, sa octahedra (one path over
  // 12 edges), tr CUBIC trails, sh ship flame, sc/sw radar, at attractor
  // (BREAK_IF_GT), lt eight nested REPEAT 2, sp empty (points only).
  const T = {
    fr: 'U6 I8,6 I9,7 S13,-1 M10,9,13 M14,2,13 A6,0,4 A6,6,14 M14,3,13 A7,1,5 A7,7,14 S11,120 S12,67 R$0 A13,11,0 A14,12,1 V13,14 A13,11,2 A14,12,3 L13,14,$1 A13,11,4 A14,12,5 L13,14,$1 A13,11,6 A14,12,7 L13,14,$1 A13,11,0 A14,12,1 L13,14,$1 X0,1,8,9,10,13,14,15 X2,3,8,9,10,13,14,15 X4,5,8,9,10,13,14,15 X6,7,8,9,10,13,14,15 E',
    ra: 'U6 S9,-1 M6,5,9 S7,120 S8,67 R$0 A9,7,0 A10,8,1 V9,10 X0,1,4,5,6,9,10,11 A9,7,0 A10,8,1 L9,10,$1 A9,7,2 A10,8,3 V9,10 X2,3,4,5,6,9,10,11 A9,7,2 A10,8,3 L9,10,$1 E',
    ru: 'U6 S14,-1 M6,5,14 S7,120 S8,67 S12,$1 S13,$2 R$0 A9,7,0 A10,8,1 V9,10 A9,7,2 A10,8,3 l12,9,10 M12,12,14 A12,12,13 X0,1,4,5,6,9,10,11 X2,3,4,5,6,9,10,11 E',
    ar: 'U8,8 R$0 S4,120 S5,67 A0,4,8 A1,5,9 A6,4,10 A7,5,11 A2,0,12 A3,1,13 A4,6,12 A5,7,13 C$1,$2 S3,-1 M2,15,3 S3,0 X8,9,14,15,2,0,1,3 X10,11,14,15,2,0,1,3 X12,13,14,15,2,0,1,3 E',
    st: 'U7 S8,1 S9,1.618034 S14,.6 S15,-.4 R$0 A7,7,8 A10,7,4 N10,10 M11,7,9 N11,11 M11,11,14 A11,11,15 M12,10,0 A12,12,5 M13,11,2 A12,12,13 M13,10,1 A13,13,6 M10,11,3 A13,13,10 P12,13,65535 E',
    gr: 'U8 S8,1.32 S9,-1 S14,6055 S15,70382 Q7 M10,2,6 A10,10,0 M11,3,6 A11,11,1 A12,10,4 A13,11,5 V12,13 M12,4,9 A12,12,10 M13,5,9 A13,13,11 l14,12,13 M14,14,9 A14,14,15 M6,6,8 E',
    la: 'U8 S8,.22 S9,1.5 R$0 M11,4,6 A11,11,2 M12,5,6 A12,12,3 M13,11,9 A13,13,0 M14,12,9 A14,14,1 V13,14 M13,11,7 A13,13,0 M14,12,7 A14,14,1 L13,14,5759 A6,6,8 E',
    ci: 'U8 S13,$2 S14,1.6 S15,-1 R$0 S8,-1.6 M11,8,4 A11,11,2 M9,11,6 A9,9,0 M11,8,5 A11,11,3 M10,11,6 A10,10,1 V9,10 R$1 M12,8,6 N12,12 M12,12,12 M12,12,7 M12,12,6 M12,12,15 M11,8,4 A11,11,2 M9,11,6 A9,9,0 M11,8,5 A11,11,3 M10,11,6 A10,10,1 M11,12,2 A9,9,11 M11,12,3 A10,10,11 L9,10,64063 A8,8,13 M11,8,4 A11,11,2 M9,11,6 A9,9,0 M11,8,5 A11,11,3 M10,11,6 A10,10,1 M11,12,2 A9,9,11 M11,12,3 A10,10,11 L9,10,64063 E M6,6,14 E',
    sa: 'U8 S13,-1 S10,1.3 M8,7,10 M9,8,13 M12,6,5 M7,6,4 M6,5,13 M14,4,13 M15,12,13 R$0 A10,1,9 V0,10 A10,0,4 A11,1,12 L10,11,65535 A10,1,8 L0,10,64063 A10,0,6 A11,1,7 L10,11,65535 A10,1,9 L0,10,64063 A10,0,14 A11,1,15 L10,11,65535 A10,1,8 L0,10,64063 M11,6,13 A10,0,11 M11,7,13 A11,1,11 L10,11,65535 A10,0,4 A11,1,12 L10,11,65535 A10,0,6 A11,1,7 L10,11,65535 A10,0,14 A11,1,15 L10,11,65535 M11,6,13 A10,0,11 M11,7,13 A11,1,11 L10,11,65535 A10,1,9 L0,10,64063 A0,0,2 A1,1,3 E',
    tr: 'U8,8 R$0 S0,0 A0,0,8 S1,0 A1,1,9 S2,0 A2,2,8 A3,9,15 S4,0 A4,4,10 A5,11,14 S6,0 A6,6,10 S7,0 A7,7,11 C$1,64327 A10,10,12 A11,11,13 E',
    sh: 'U4 V0,1 L2,3,64327',
    sc: 'S8,120 S9,67 S10,20 S11,11.05 S12,-20 S13,-11.05 S14,1.45 R$0 A0,8,10 S1,67 A2,8,10 A3,9,11 A4,8,11 A5,9,10 S6,120 A7,9,10 C8,6055 S0,120 A1,9,10 A2,8,13 A3,9,10 A4,8,12 A5,9,11 A6,8,12 S7,67 C8,6055 A0,8,12 S1,67 A2,8,12 A3,9,13 A4,8,13 A5,9,12 S6,120 A7,9,12 C8,6055 S0,120 A1,9,12 A2,8,11 A3,9,12 A4,8,10 A5,9,13 A6,8,10 S7,67 C8,6055 M10,10,14 M11,11,14 M12,12,14 M13,13,14 E S0,40 S1,67 V0,1 S0,200 L0,1,6055 S0,120 S1,7 V0,1 S1,127 L0,1,6055',
    sw: 'S8,120 S9,67 V8,9 I0,0 I1,1 L0,1,65535 I2,2 I3,3 I4,4 I5,5 S10,65535 S11,129862 S12,-1 R$0 p10,2,3 A2,2,4 A3,3,5 M10,10,12 A10,10,11 E',
    at: 'U8 S12,1.5707963267948966 S15,-1 S8,.1 S9,.1 R255 R$0 M10,0,9 N10,10 M11,1,8 A11,11,12 N11,11 M11,11,15 A10,10,11 M11,2,8 N11,11 A8,10,14 M10,3,9 A10,10,12 N10,10 M10,10,15 A9,11,10 E M10,8,6 A10,10,4 M11,9,6 A11,11,5 L10,11,64063 A13,13,12 B13,7 E',
    lt: 'U8 S8,$0 S9,$1 S10,-1 S11,5759 S12,71294 R2 R2 R2 R2 R2 R2 R2 R2 l11,8,9 A8,8,0 M0,0,10 E A9,9,1 M1,1,10 E A8,8,2 M2,2,10 E A9,9,3 M3,3,10 M11,11,10 A11,11,12 E A8,8,4 M4,4,10 E A9,9,5 M5,5,10 E A8,8,6 M6,6,10 E A9,9,7 M7,7,10 E',
    sp: 'S0,0'
  };
  const Q14 = 16384, rnd = Math.round, M = Math;
  function affine(x, y, s, t, tx, ty, col) {
    const c = s * M.cos(t) * Q14, n = s * M.sin(t) * Q14;
    return {kind: 'affineQ14Points', x: x, y: y, color: col,
      coeff: [rnd(c), rnd(-n), rnd(n), rnd(c), tx * Q14, ty * Q14]};
  }
  const SHIP = [0, -12, 2, -7, 3, -3, 12, 3, 12, 6, 3, 4, 2, 7, 5, 10, 0, 8, -5, 10, -2, 7,
    -3, 4, -12, 6, -12, 3, -3, -3, -2, -7, 0, -12];
  function points(b, n) {
    const x = [], y = [];
    for (let i = 0; i < n; ++i) {
      const r = 300 * M.pow(.974, i);
      if (b < 8) { x.push(SHIP[2 * i]); y.push(SHIP[2 * i + 1]); }
      else { x.push(rnd(-r * M.cos(i * .35))); y.push(rnd(-r * M.sin(i * .35))); }
    }
    // Ship: 8 roll bins; spiral: rotating by one point step equals shrinking.
    return b < 8 ? affine(x, y, 1.7, (b - 3.5) * .14, 120, 110, WH) :
      affine(x, y, 1.9, (b - 8) * .175, 120, 67, OR);
  }
  const Q0 = [[1.2, .95], [-.8, .95], [-.8, -1.55], [1.2, -1.55]];
  const cmul = (p, z) => [p[0] * z[0] - p[1] * z[1], p[0] * z[1] + p[1] * z[0]];
  const zp = (r, d, e) => [M.pow(r, e) * M.cos(d * e), M.pow(r, e) * M.sin(d * e)];
  const lerp = (p, q, f) => [p[0] + (q[0] - p[0]) * f, p[1] + (q[1] - p[1]) * f];
  // TWIST: frames, rails, arches, planks, 2nd rails, 2 wall courses.
  // LIMIT: frames, rails, arches, planks, ceiling and wall rungs.
  function corrList(k, lim) {
    const n = k.n, m = k.m;
    return [['fr', n - k.kf, VI], ['ra', n, GR], ['ar', n - k.ka, k.s, WH], ['ru', n * m, OR, OR + WH]]
      .concat(lim ? [['ru', n * m, VI, VI + BL], ['ru', (n - 1) * m, GR, GR + BL]] :
        [['ra', n, GR], ['ra', n, BL], ['ra', n, BL]]);
  }
  // The corridor: every point of a cross-section is multiplied by z per frame.
  function corrDraws(k, t, len, sp, lim) {
    const r = k.r, w = M.sin(M.PI * t / len), dl = k.D / k.n * w * w;
    const ph = (t * sp) % 1, g = M.pow(r, 1 - ph) * 265;
    const an = .5 * M.sin(2 * M.PI * t / len) - ph * dl;
    const c = M.cos(an) * g, s = M.sin(an) * g;
    const P = Q0.map(q => [q[0] * c - q[1] * s, q[0] * s + q[1] * c]);
    const z = zp(r, dl, 1), zf = zp(r, dl, 1 / k.m), zk = zp(r, dl, k.kf), za = zp(r, dl, k.ka);
    const A = cmul([(P[2][0] - P[1][0]) / 2, (P[2][1] - P[1][1]) / 2], za);
    const d = [[0, cmul(P[0], zk).concat(cmul(P[1], zk), cmul(P[2], zk), z)],
      [1, P[0].concat(P[1], z)], [3, P[0].concat(P[1], zf)]];
    if (lim) d.push([4, P[2].concat(P[3], zf)], [5, cmul(P[0], z).concat(cmul(P[3], z), zf)]);
    else d.push([4, P[2].concat(P[3], z)], [5, lerp(P[1], P[2], 1 / 3).concat(lerp(P[0], P[3], 1 / 3), z)],
      [6, lerp(P[1], P[2], 2 / 3).concat(lerp(P[0], P[3], 2 / 3), z)]);
    d.push([2, cmul(P[2], za).concat(cmul(P[3], za), A, z)]);
    return d;
  }
  // Roll of the ZENITH ship (a barrel roll in frames 40..63) and pitch.
  function flight(t) {
    const T = 2 * M.PI * t / 96;
    let roll = .55 * M.sin(T);
    if (t >= 40 && t < 64) { const u = (t - 40) / 24; roll += 2 * M.PI * u * u * (3 - 2 * u); }
    const c = M.cos(roll), s = M.sin(roll), hz = 8 * M.sin(2 * T);
    return {roll: roll, c: c, s: s, H: [120 - hz * s, 67 + hz * c, -s, c]};
  }
  const A2 = {
    names: ['TWIST', 'ZENITH', 'LIMIT'], len: [128, 96, 96],
    // Plan list of scene i: [template, args...]; sh and sp carry points.
    plans: function (i, tier) {
      const k = KN[tier], z = k.z, l = k.l;
      if (!i) return corrList(k.c, 0);
      if (i === 1) {
        const p = [['st', z.st], ['gr'], ['la', z.ln], ['ci', z.cr, z.cc, 3.2 / z.cc], ['sa', z.sa],
          ['tr', z.sa, z.ts]];
        for (let b = 0; b < 8; ++b) p.push(['sh', b]);
        return p.concat([['sc', z.rr], ['sw', z.sa]]);
      }
      return corrList(l, 1).concat([['gr'], ['la', l.ln], ['at', l.ai],
        ['lt', 120 - 22.5 * l.lt, 67 - 22.5 * l.lt], ['sp', 8], ['sp', 9], ['sc', 4], ['sw', 4]]);
    },
    // {b: backdrop, s: surface, d: [[plan, inputs]...]}
    frame: function (i, tier, t) {
      const k = KN[tier], z = k.z, l = k.l;
      if (!i) return {b: 0, s: 0, d: corrDraws(k.c, t, 128, 1 / 6, 0)};
      const f = flight(i === 1 ? t : t * 1.5 % 96), c = f.c, s = f.s, H = f.H;
      const x = 34 + 24 * M.sin(t * .09) - 120, y = 28 + 10 * M.cos(t * .05) - 67;
      const P = [120 + x * c - y * s, 67 + x * s + y * c], st = [46 * c - 6 * s, 46 * s + 6 * c];
      const u = [c, s], gl = i === 1 ? z.gl : l.gl, ln = i === 1 ? z.ln : l.ln, g = i === 1 ? 1 : 6;
      if ((t & 3) === 3) return {b: 0, s: 1, d: [[i === 1 ? 14 : 12, []], [i === 1 ? 15 : 13,
        [120 + 60 * M.cos(t * .3), 67 + 60 * M.sin(t * .3), 120 + (P[0] - 120) * .4, 67 + (P[1] - 67) * .4,
          st[0] * .4, st[1] * .4]]]};
      const d = [[g, H.concat([260 * c, 260 * s, 2.2 * M.pow(1.32, (t * .25) % 1), gl])],
        [g + 1, H.concat(u, [-(ln - 1) * .11 - ((t * .04) % 1) * .22, i === 1 ? 150 : l.dn])]];
      if (i === 1) {
        d.unshift([0, [190 * c, 190 * s, -70 * s, 70 * c, t * .05, 120, 67]]);
        d.push([3, H.concat(u, [4.5, M.min(1, t / 70) * 2.2])],
          [4, P.concat(st, [10 * M.cos(t * .3), 10 * M.sin(t * .3), .35, 10])]);
        if (t % 16 < 5) d.push([5, [120, 100].concat(P, st, [20, -40])]);
        const b = M.max(0, M.min(7, rnd(-M.atan2(s, c) / .14 + 3.5)));
        d.push([6 + b, [120, 120, 120, 123 + (t % 3) * 2]]);
        return {b: 0, s: 0, d: d};
      }
      const q = [];
      for (let j = 0; j < 8; ++j) q.push((3 << (j >> 1)) * l.lt * (1 + .35 * M.sin(t * .1 * (j + 1))));
      d.push([10 + (t & 1), []]);
      return {b: 0, s: 0, d: d.concat(corrDraws(l, t, 96, 1 / 4, 1), [[9, q],
        [8, [1.4 + .2 * M.sin(t * .05), -2.3, 2.4, -2.1 + .2 * M.cos(t * .04), 120, 67, 36,
          (l.ao - .5) * M.PI / 2]]])};
    }
  };

  const NAMES = ['NEWS', 'NEWS', 'NEWS'].concat(A2.names), LEN = [16, 16, 16].concat(A2.len);
  // One thunk per plan: the loader builds one program array at a time.
  function specs(s, tier) {
    return (s > 2 ? A2.plans(s - 3, tier) : [0, 1, 2, 3, 4]).map(e => () => s < 3 ?
      [program(s, e), pointBatch(s, e)] :
      [prog(T[e[0]], e.slice(1)), e[0] === 'sh' || e[0] === 'sp' ? points(e[1], e[0] === 'sh' ? 17 : 120) : null]);
  }
  function frameOf(s, tier, t) {
    if (s > 2) return A2.frame(s - 3, tier, t);
    const d = [];
    for (let l = 0; l < 5; ++l) d.push([l, inputs(s * 16 + t, l)]);
    return {b: backdrops[s], s: 0, d: d};
  }

  globalThis.procMegademo = {
    program: program,
    inputs: inputs,
    pointBatch: pointBatch,
    backdrop: function (frame) {
      checkFrame(frame);
      return backdrops[Math.floor(frame / 16)];
    },
    names: NAMES, len: LEN, specs: specs, frame: frameOf, tiers: KN.length
  };

  if (typeof pocket !== 'undefined' && pocket.kasane && pocket.kasane.procedural) {
    const V = pocket.kasane, H = V.procedural, G = V.grid;
    const res0 = H.resource(), surf1 = H.createSurface(), res1 = H.resource(surf1);
    // Both surfaces' frames (5 x 10 KB) now, while the heap still has holes
    // that size; ZENITH's first beginFrame(surf1) found none on the device.
    H.beginFrame(0, surf1);
    H.beginFrame(0);
    // Registered image-to-image resizes share the native span/PIE path: the
    // settled monitor (bilinear) and the radar thumbnail (nearest).
    const monRes = G.resource(G.registerResizeSource({source: res0, width: 112, height: 63}));
    const thumbRes = G.resource(G.registerResizeSource({source: res1, width: 60, height: 34,
      sampling: 'nearest'}));
    const pets = V.resource('pets'), munch = V.pixel.open(32, 24);
    const lives = V.cache.create([{bounds: [0, 0, 9, 5], color: 0x14f53cff},
      {bounds: [3, 1, 6, 4], color: 255}]);
    // Munching squares: ((x ^ y) * t) & violet; the mask register is alpha too.
    const MC = new Uint16Array([2, 0, 0, 0, 0, 3, 1, 0, 0, 0, 12, 2, 0, 1, 0, 4, 3, 0, 0, 0,
      8, 4, 2, 3, 0, 4, 5, 1, 0, 0, 10, 6, 4, 5, 0]);
    const MP = new Uint16Array(8);
    MP[1] = VI;
    const FULL = [0, 0, 240, 135], MON = [64, 20, 176, 83], RAD = [4, 84, 52, 132], ZOOM = 44;
    // Loader: the next scene registers from LOAD[0] frames before the switch,
    // LOAD[1] plans a frame, while the internal heap has LOAD[2] bytes free
    // (a registration dips it by up to 14 KB on the device); the rest
    // registers over the scene's first frames, which skip unloaded plans.
    const LOAD = [16, 1, 22528], MEM = pocket.memory;
    // The Act I news set: [x0, y0, x1, y1, rgba, caption].
    const NEWS = [[0, 0, 240, 15, 0x061521ff], [8, 2, 180, 14, 0xd7f4ffff, 'POCKET NEWS  /  STUDIO 01'],
      [11, 20, 17, 92, 0x40a8b8ff], [20, 28, 25, 86, 0x21536cff], [181, 21, 231, 88, 0x0b1c2bff],
      [186, 30, 228, 44, 0xff795cff, 'ON AIR'], [186, 49, 224, 51, 0x3d99aaff],
      [186, 58, 217, 60, 0x285f77ff], [186, 67, 226, 69, 0x285f77ff], [60, 16, 180, 87, 0x02070bff],
      [62, 18, 178, 85, 0x8aa7b1ff], [0, 106, 240, 135, 0x06111eff], [0, 106, 240, 109, 0x45cddaff],
      [8, 113, 44, 128, 0xd93436ff], [12, 115, 41, 127, 0xffffffff, 'LIVE'],
      [50, 112, 233, 124, 0xffffffff, 'MEGADEMO  /  THE CANVAS REPORT'],
      [50, 124, 232, 134, 0x80d9e8ff, 'ENTER FULL SCREEN   ESC HOME']];
    let tier = KN.length - 1, scene = 0, t = 0, tick = 0, live = [], pend = [], next = null, loaded = 0;
    let img, mon, hud = [], R = {}, anim = null, built = -1, vis = false, shown = true;
    let zoom = 0, target = 0, held = 0, fixed = false;
    const drop = h => { for (let i = 0; i < h.length; ++i) H.unregister(h[i]); };
    const reg = th => { const p = th(); ++loaded; return p[1] ? H.register(p[0], p[1]) : H.register(p[0]); };
    function prefetch(s, max) {
      if (next && (next.s !== s || next.k !== tier)) { drop(next.h); next = null; }
      if (!next) next = {s: s, k: tier, h: [], sp: specs(s, tier)};
      while (max-- > 0 && next.h.length < next.sp.length && MEM.info().internalFreeBytes >= LOAD[2])
        next.h.push(reg(next.sp[next.h.length]));
    }
    // fresh: drop the prefetched set too (tier change, recovery). The old set
    // goes before the rest registers: they overlap only as far as the
    // memory gate let the prefetch run.
    function enter(s, fresh) {
      if (fresh && next) { drop(next.h); next = null; }
      prefetch(s, 0);
      const old = live.length;
      drop(live); live = next.h; pend = next.sp.slice(live.length); next = null; scene = s; t = 0;
      console.log('MEGADEMO SCENE ' + NAMES[s] + ' tier=' + tier + ' plans=' + live.length +
        ' freed=' + old + ' registered=' + loaded);
    }
    const spin = tx => R.radar.animate(tx, {from: {bounds: RAD, rotation: 0},
      to: {bounds: RAD, rotation: 360 * KN[tier].q[scene === 4 ? 0 : 2]}, durationMs: 4000,
      easing: 'linear', repeat: 'loop'});
    const boundsAt = step => {
      const u = step / ZOOM, e = u * u * (3 - 2 * u);
      return FULL.map((v, j) => rnd(v + (MON[j] - v) * e));
    };
    function build(tx) {
      hud = []; R = {}; anim = null; vis = false; shown = true;
      const add = r => { hud.push(r); return r; };
      if (scene < 3) {
        tx.background(0x07101cff);
        tx.gradient({bounds: [0, 0, 240, 90], axis: 'x', from: 0x0d263bff, to: 0x18384aff});
        for (let i = 0; i < NEWS.length; ++i) {
          const e = NEWS[i];
          if (e[5]) tx.text({bounds: e.slice(0, 4), text: e[5], font: 'caption', color: e[4]});
          else tx.rect({bounds: e.slice(0, 4), color: e[4]});
        }
      } else {
        // An Apple-style monitor on a desk, seen only while zoomed out.
        tx.background(255);
        tx.gradient({bounds: [0, 0, 240, 100], axis: 'y', from: 0x100828ff, to: 0x3a2050ff, dither: true});
        tx.rect({bounds: [0, 100, 240, 135], color: 0x4a3020ff});
        tx.roundRect({bounds: [52, 10, 188, 96], radius: 8, color: 0xd8ccb0ff});
        tx.strokeRect({bounds: [60, 16, 180, 87], width: 2, color: 0x6a6050ff});
        R.pet = tx.image({resource: pets, bounds: [196, 72, 228, 104], variant: 3});
        tx.text({bounds: [8, 110, 232, 124], text: 'ACT II  ' + NAMES[scene] + '  UP/DOWN LOAD',
          font: 'caption', color: 0xd8ccb0ff});
      }
      // The opaque image is last: at full size it exactly covers the set.
      img = tx.image({resource: res0, bounds: boundsAt(zoom), clip: FULL, sourceWidth: 240, sourceHeight: 135});
      mon = tx.image({resource: monRes, bounds: MON, clip: FULL, sourceWidth: 112, sourceHeight: 63});
      mon.setVisible(tx, false);
      if (scene === 3) R.title = add(tx.text({bounds: [6, 4, 234, 24], text: 'TWISTED CORRIDOR',
        font: 'display', color: 0xfa3fffff}));
      if (scene < 4) return;
      R.score = add(tx.text({bounds: [4, 2, 150, 13], text: NAMES[scene], capacity: 20,
        font: 'caption', color: 0xffffffff}));
      R.lamp = add(tx.roundRect({bounds: [226, 3, 236, 11], radius: 3, color: 0xfb47ffff}));
      R.radar = add(tx.image({resource: res1, bounds: RAD, clip: FULL, sourceX: 60, sourceY: 7,
        sourceWidth: 120, sourceHeight: 120}));
      add(tx.strokeRect({bounds: [3, 83, 53, 133], width: 1, color: 0x17a7ffff}));
      anim = spin(tx);
      if (scene === 4) {
        R.thumb = add(tx.image({resource: thumbRes, bounds: [176, 99, 236, 133], clip: FULL}));
        add(tx.image({resource: munch, bounds: [172, 16, 236, 64], clip: FULL, scale: 2}));
        R.lives = [];
        for (let i = 0; i < 3; ++i) R.lives.push(add(tx.instantiate(lives, {offset: [160 + i * 12, 4]})));
      } else {
        R.plate = add(tx.rect({bounds: [60, 118, 236, 133], color: 255}));
        add(tx.rect({bounds: [62, 120, 234, 131], color: 0xfa3fffff, opacity: 90}));
        tx.group(R.plate, 2, 170);
        R.meter = add(tx.gradient({bounds: [64, 128, 65, 130], axis: 'x', from: 0x17a7ffff, to: 0xfb47ffff}));
        R.stat = add(tx.text({bounds: [64, 119, 234, 128], text: 'LOAD', capacity: 40,
          font: 'caption', color: 0xffffffff}));
      }
    }
    function patch(tx, f) {
      img.setRect(tx, boundsAt(zoom));
      const fix = zoom === ZOOM, on = zoom === 0;
      if (fix !== vis) { img.setVisible(tx, !fix); mon.setVisible(tx, fix); vis = fix; }
      if (on !== shown) {
        for (let i = 0; i < hud.length; ++i) hud[i].setVisible(tx, on);
        if (anim) { if (on) anim = spin(tx); else anim.finish(tx); }
        shown = on;
      }
      if (scene < 3) return;
      if (!on && !(t & 3)) R.pet.setImageFrame(tx, 3, (t >> 2) % 6);
      if (scene === 3) {
        R.title.setReveal(tx, M.min(16, t >> 1));
        const e = M.min(1, t / 20), w = 20 + 100 * e, h = 12 + 56 * e;
        img.setClip(tx, [rnd(120 - w), rnd(67 - h), rnd(120 + w), rnd(68 + h)]);
        return;
      }
      if (!(t & 7)) R.score.setText(tx, NAMES[scene] + ' ' + String(1e6 + tick * 10).slice(1));
      R.lamp.setColor(tx, t & 8 ? 0xfb47ffff : 0x167fffff);
      if (scene === 4) {
        R.thumb.setRotation(tx, -flight(t).roll * 57.3 * KN[tier].q[1]);
        for (let i = 0; i < 3; ++i) R.lives[i].place(tx, {offset: [160 + i * 12, 4 + ((t + i * 3) >> 2) % 2]});
        R.lives[2].setVisible(tx, on && t < 48);
      } else {
        const n = f ? f.d.length : 0;
        R.meter.setRect(tx, [64, 128, 65 + rnd(169 * n / 12), 130]);
        R.stat.setText(tx, 'TIER ' + tier + '  PLANS ' + live.length + '  DRAWS ' + n);
      }
      if (t === 90 && anim && on) { anim.stop(tx); console.log('MEGADEMO ANIM ' + anim.poll()); }
    }
    enter(0);
    while (pend.length) live.push(reg(pend.shift()));
    globalThis.frame = function (buttons) {
      buttons |= 0;
      const press = buttons & ~held;
      held = buttons;
      if (press & 0x4000) target = target ? 0 : ZOOM;
      zoom += zoom < target ? 1 : zoom > target ? -1 : 0;
      let f = null;
      try {
        // UP/DOWN: load tier. LEFT/RIGHT: previous/next scene.
        const k = tier + (press & 0x10 ? 1 : press & 0x40 ? -1 : 0);
        if (k !== tier && k >= 0 && k < KN.length) { tier = k; enter(scene, 1); }
        if (press & 0xa0) enter((scene + (press & 0x20 ? 1 : NAMES.length - 1)) % NAMES.length);
        else if (t >= LEN[scene]) enter((scene + 1) % NAMES.length);
        f = frameOf(scene, tier, t);
        if (f.s) H.beginFrame(f.b, surf1);
        else H.beginFrame(f.b);
        for (let i = 0; i < f.d.length; ++i) {
          const h = live[f.d[i][0]];
          if (h) H.draw(h, f.d[i][1]);
        }
        H.commit();
        // A plan decodes in ~13 ms on the device: one a frame, this scene's
        // first, then the next scene's over the last frames.
        let b = LOAD[1];
        for (; b > 0 && pend.length; --b) live.push(reg(pend.shift()));
        if (b && t >= LEN[scene] - LOAD[0]) prefetch((scene + 1) % NAMES.length, b);
      } catch (e) {
        if (!tier) throw e;
        --tier;
        console.log('MEGADEMO DEGRADE tier=' + tier + ' ' + e);
        enter(scene, 1);
        f = null;
      }
      if (scene === 4) {
        MP[0] = 257 + tick * 3;
        V.pixel.stage(MC, MP, 6, 5);
      }
      const key = scene < 3 ? 0 : scene + 8 * tier;
      if (built !== key) {
        V.replace(build);
        built = key;
      } else {
        // The set built at t = 0 has been presented by now.
        if (t === 1) console.log('MEGADEMO VIEW ' + NAMES[scene] + ' commands=' + V.stats().displayed.commands);
        V.patch(tx => patch(tx, f));
      }
      if ((zoom === ZOOM) !== fixed) {
        fixed = !fixed;
        console.log('MEGADEMO IMAGE ' + (fixed ? 'FIXED_PIE' : 'DYNAMIC_STRETCH'));
      }
      ++t; ++tick;
    };
  }
})();
