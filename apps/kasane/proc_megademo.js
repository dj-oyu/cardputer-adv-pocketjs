// MEGADEMO: NIGHT SIGNAL. A room, a cat, a monitor; the story plays on the
// screen: Act I news (3x16 frames, held twice), TWIST, ZENITH, CORE, LOST.
// Story: docs/apps/megademo-story.md; limits: docs/kasane/megademo-limit-scenes.md
(function () {
  'use strict';
  const colors = [
    [0x07ff, 0x3dff, 0xb81f, 0xffff],
    [0xfde0, 0xfb00, 0x07ff, 0xffff],
    [0xf81f, 0x781f, 0x07ff, 0xffff]
  ];
  const backdrops = [0x080c, 0x000d, 0x100b];
  const f32 = Math.fround;

  // Act I layers in the text form decoded by prog() below: a gate, a
  // highway, rising bars, a sine, scanlines and a faceted crystal.
  const ACT1 = ['U2 S2,$0 S3,$1 S4,$2 S5,$3 S6,$4 S7,-7 R$5 V0,2 L1,2,$6 L1,3,$6 L0,3,$6 L0,2,$6 \
A0,0,4 A1,1,5 A2,2,6 A3,3,7 E', 'U2 S2,$0 S3,134 S4,23 S5,4.8 R12 V1,2 L0,3,$1 A0,0,4 A1,1,5 E',
    'S0,0 S1,239 I2,0 S3,1.5 S4,1.47 R10 V0,2 L1,2,$0 A2,2,3 M3,3,4 E',
    'S0,0 S1,4 S2,$0 I3,0 S4,$1 S5,$2 R60 M6,0,2 A6,6,3 N6,6 M7,6,4 A7,7,5 L0,7,$3 A0,0,1 E',
    'I0,0 S1,29 S2,0 S3,239 I4,1 S6,2 R4 V2,0 L3,0,$2 A5,0,6 V2,5 L3,5,$0 A0,0,1 A2,2,4 A3,3,4 E \
S0,88 S1,152 S2,32 S3,98 S4,120 S5,65 I6,2 I7,3 V4,2 L1,5,$2 L4,3,$2 L0,5,$2 L4,2,$2 V4,2 \
L4,3,$3 V0,5 L1,5,$3 V4,2 A4,4,6 A2,2,7 L4,2,$3 V1,5 A1,1,6 A5,5,7 L1,5,$3 S4,120 V4,3 \
A4,4,6 A3,3,7 L4,3,$3 S5,65 V0,5 A0,0,6 A5,5,7 L0,5,$3 V4,2 L1,5,$1 L4,3,$1 L0,5,$1 L4,2,$1'];
  function program(phase, layer) {
    const n = colors[phase], a = phase === 1;
    return prog(ACT1[layer], [
      [a ? 42 : 5, a ? 125 : 129, a ? 13 : 12, a ? -13 : -12, a ? 3 : 7, a ? 6 : 8, n[0]],
      [a ? 64 : 68, n[1]], [n[2]],
      [f32(f32(.073) + f32(phase * .015)), a ? 7 : 13, phase === 2 ? 42 : 28, n[3]], n][layer]);
  }

  function inputs(frame, layer) {
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
  // k: CORE (r rays a draw, d ray draws, n noise lines); q: rotate ZENITH's
  // radar, ~23 ms of band re-renders a frame on the device. Derivation:
  // docs/kasane/megademo-limit-scenes.md, megademo-device-limits.md,
  // docs/apps/megademo-story.md (CORE).
  // TWIST and ZENITH hold the display rate at HEAVY's load, so MID shares it.
  const CH = {n: 26, r: .8, m: 3, s: 8, kf: 2, ka: 3, D: 6},
    ZH = {st: 60, gl: 14, ln: 15, cr: 3, cc: 8, sa: 4, ts: 16, rr: 4};
  const KN = [
    {q: 0, c: {n: 12, r: .7, m: 2, s: 4, kf: 2, ka: 3, D: 4}, z: {st: 24, gl: 8, ln: 7, cr: 2, cc: 4, sa: 2, ts: 6, rr: 2},
     k: {r: 20, d: 1, n: 12}},
    {q: 0, c: CH, z: ZH, k: {r: 40, d: 1, n: 24}},
    {q: 1, c: CH, z: ZH, k: {r: 40, d: 2, n: 40}}
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
  // 12 edges), tr CUBIC trails, sh ship flame, sc/sw radar, ln a line; CORE:
  // tri a triangle (colour from input 6), qd a closed quad, sg two segments,
  // ry rays round a centre (REPEAT_REG), nz scan noise (BREAK_IF_GT).
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
    ln: 'U4 V0,1 L2,3,$0',
    tri: 'U7 V0,1 l6,2,3 l6,4,5 l6,0,1',
    qd: 'U8 V0,1 L2,3,$0 L4,5,$0 L6,7,$0 L0,1,$0',
    sg: 'U8 V0,1 L2,3,$0 V4,5 L6,7,$0',
    ry: 'U8 S8,1.5707963 Q5 A9,4,8 N9,9 N10,4 M11,9,2 A11,11,0 M12,10,2 A12,12,1 V11,12 M11,9,3 A11,11,0 M12,10,3 A12,12,1 l7,11,12 A4,4,6 E',
    nz: 'U3 S3,0 S4,1 S5,67 S6,120 S7,7.31 S8,3.97 R$0 A3,3,4 B3,1 M9,3,7 A9,9,0 N9,9 M9,9,5 A9,9,5 M10,3,8 A10,10,0 N10,10 M10,10,6 A10,10,6 V10,9 A11,10,2 L11,9,33808 E',
    sc: 'S8,120 S9,67 S10,20 S11,11.05 S12,-20 S13,-11.05 S14,1.45 R$0 A0,8,10 S1,67 A2,8,10 A3,9,11 A4,8,11 A5,9,10 S6,120 A7,9,10 C8,6055 S0,120 A1,9,10 A2,8,13 A3,9,10 A4,8,12 A5,9,11 A6,8,12 S7,67 C8,6055 A0,8,12 S1,67 A2,8,12 A3,9,13 A4,8,13 A5,9,12 S6,120 A7,9,12 C8,6055 S0,120 A1,9,12 A2,8,11 A3,9,12 A4,8,10 A5,9,13 A6,8,10 S7,67 C8,6055 M10,10,14 M11,11,14 M12,12,14 M13,13,14 E S0,40 S1,67 V0,1 S0,200 L0,1,6055 S0,120 S1,7 V0,1 S1,127 L0,1,6055',
    sw: 'S8,120 S9,67 V8,9 I0,0 I1,1 L0,1,65535 I2,2 I3,3 I4,4 I5,5 S10,65535 S11,129862 S12,-1 R$0 p10,2,3 A2,2,4 A3,3,5 M10,10,12 A10,10,11 E',
  };
  const Q14 = 16384, rnd = Math.round, M = Math;
  function affine(x, y, s, t, tx, ty, col) {
    const c = s * M.cos(t) * Q14, n = s * M.sin(t) * Q14;
    return {kind: 'affineQ14Points', x: x, y: y, color: col,
      coeff: [rnd(c), rnd(-n), rnd(n), rnd(c), tx * Q14, ty * Q14]};
  }
  // The cat's ship: a head with two ears, seen from behind.
  const SHIP = [0, -4, 5, -12, 7, -5, 13, 0, 10, 5, 5, 8, 2, 6, 0, 9, -2, 6, -5, 8, -10, 5,
    -13, 0, -7, -5, -5, -12, 0, -4];
  // The ship in 8 roll bins (a rotated sprite table).
  function points(b) {
    const x = [], y = [];
    for (let i = 0; i < SHIP.length; i += 2) { x.push(SHIP[i]); y.push(SHIP[i + 1]); }
    return affine(x, y, 2, (b - 3.5) * .14, 120, 110, WH);
  }
  const Q0 = [[1.2, .95], [-.8, .95], [-.8, -1.55], [1.2, -1.55]];
  const cmul = (p, z) => [p[0] * z[0] - p[1] * z[1], p[0] * z[1] + p[1] * z[0]];
  const zp = (r, d, e) => [M.pow(r, e) * M.cos(d * e), M.pow(r, e) * M.sin(d * e)];
  const lerp = (p, q, f) => [p[0] + (q[0] - p[0]) * f, p[1] + (q[1] - p[1]) * f];
  // TWIST: frames, rails, arches, planks, 2nd rails, 2 wall courses.
  function corrList(k) {
    const n = k.n;
    return [['fr', n - k.kf, VI], ['ra', n, GR], ['ar', n - k.ka, k.s, WH], ['ru', n * k.m, OR, OR + WH],
      ['ra', n, GR], ['ra', n, BL], ['ra', n, BL]];
  }
  // The corridor: every point of a cross-section is multiplied by z per frame.
  function corrDraws(k, t, len, sp) {
    const r = k.r, w = M.sin(M.PI * t / len), dl = k.D / k.n * w * w;
    const ph = (t * sp) % 1, g = M.pow(r, 1 - ph) * 265;
    const an = .5 * M.sin(2 * M.PI * t / len) - ph * dl;
    const c = M.cos(an) * g, s = M.sin(an) * g;
    const P = Q0.map(q => [q[0] * c - q[1] * s, q[0] * s + q[1] * c]);
    const z = zp(r, dl, 1), zf = zp(r, dl, 1 / k.m), zk = zp(r, dl, k.kf), za = zp(r, dl, k.ka);
    const A = cmul([(P[2][0] - P[1][0]) / 2, (P[2][1] - P[1][1]) / 2], za);
    const d = [[0, cmul(P[0], zk).concat(cmul(P[1], zk), cmul(P[2], zk), z)],
      [1, P[0].concat(P[1], z)], [3, P[0].concat(P[1], zf)]];
    d.push([4, P[2].concat(P[3], z)], [5, lerp(P[1], P[2], 1 / 3).concat(lerp(P[0], P[3], 1 / 3), z)],
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
  // CORE: an icosahedral shell opens round a pulsing core (cube and
  // octahedron); the climax adds grey flashes, scan noise and a short dolly
  // zoom, each eased in cubically towards frame 100. The 3D is projected
  // here; the plans only draw lines.
  // Icosahedron: vertices as sign/golden-ratio codes (i = -1, p/q = +/-phi),
  // faces as hex vertex triples; the cube and octahedron are made from bits.
  const IV = [], IFS = '0b505101707a0ab1595b4ba2a7671839434232636838949524b62a867981';
  {
    const p = (1 + M.sqrt(5)) / 2, n = M.sqrt(1 + p * p), C = {i: -1, '0': 0, '1': 1, p: p, q: -p};
    const S = 'ip01p0iq01q00ip01p0iq01qp0ip01q0iq01';
    for (let i = 0; i < 36; ++i) IV.push(C[S[i]] / n);
  }
  const ramp = (t, a, b) => M.max(0, M.min(1, (t - a) / (b - a))), smooth = u => u * u * (3 - 2 * u);
  // Projection state for this frame: yaw, pitch, camera distance D.
  let Ycy, Ysy, Pcp, Psp, Dd;
  // Rotate (yaw, then pitch), project at scale s, append x, y to o; returns z
  // (> 0 is away from the camera). A petal past the lens must not blow up.
  function pj(x, y, z, s, o) {
    const x1 = x * Ycy + z * Ysy, z0 = z * Ycy - x * Ysy, y1 = y * Pcp - z0 * Psp, z1 = y * Psp + z0 * Pcp;
    const f = s * Dd / M.max(Dd + z1, Dd * .4);
    o.push(120 + x1 * f, 67 + y1 * f);
    return z1;
  }
  // A quad (or two segments) through vertices named by hex digits: cube
  // vertex i is the bits of i, octahedron vertex i the axis i % 3 signed by i < 3.
  function quad(plan, ids, oct, s) {
    const o = [];
    for (let j = 0; j < 4; ++j) {
      const i = parseInt(ids[j], 16);
      if (oct) pj(i % 3 ? 0 : i < 3 ? 1 : -1, i % 3 === 1 ? i < 3 ? 1 : -1 : 0, i % 3 === 2 ? i < 3 ? 1 : -1 : 0, s, o);
      else pj(i & 1 ? 1 : -1, i & 2 ? 1 : -1, i & 4 ? 1 : -1, s, o);
    }
    return [plan, o];
  }
  function core(k, t) {
    const crit = ramp(t, 60, 100), dz = ramp(t, 84, 100) ** 2, gone = smooth(ramp(t, 100, 110));
    const sc = (18 + 44 * smooth(ramp(t, 0, 40))) * (1 - gone);
    const op = smooth(ramp(t, 40, 56)) + .6 * smooth(ramp(t, 76, 100)), tw = op * (.5 + 1.5 * crit);
    const ya = t * .03 + 2 * crit ** 3, pa = .5 + t * .011, d = [];
    // Grey flashes: one frame each time 9 u^3 crosses an integer, u over 60..100.
    const fl = t === 60 || t > 60 && t <= 100 && (9 * crit ** 3 | 0) > (9 * ramp(t - 1, 60, 100) ** 3 | 0);
    Ycy = M.cos(ya); Ysy = M.sin(ya); Pcp = M.cos(pa); Psp = M.sin(pa); Dd = 3.2 - 1.6 * dz;
    for (let i = 0; i < 20; ++i) {
      // Shrink each face, push it out along its normal, twist it like a petal.
      let cx = 0, cy = 0, cz = 0, zs = 0;
      for (let j = 0; j < 3; ++j) {
        const v = 3 * parseInt(IFS[3 * i + j], 16);
        cx += IV[v] / 3; cy += IV[v + 1] / 3; cz += IV[v + 2] / 3;
      }
      const l = op * .8 / M.hypot(cx, cy, cz), a = tw * (i & 1 ? 1 : -1), ca = M.cos(a), sa = M.sin(a), o = [];
      for (let j = 0; j < 3; ++j) {
        const v = 3 * parseInt(IFS[3 * i + j], 16), h = 1 - .35 * op;
        const x = cx + (IV[v] - cx) * h + cx * l, y = cy + (IV[v + 1] - cy) * h + cy * l, z = cz + (IV[v + 2] - cz) * h + cz * l;
        zs += pj(x * ca + z * sa, y, z * ca - x * sa, sc, o);
      }
      o.push(zs > 0 ? BL : i % 3 ? VI : WH);
      d.push([0, o]);
    }
    // The core: cube (green) and octahedron (orange), counter-rotating, pulsing.
    const cs = sc * smooth(ramp(t, 40, 56)) * (1 + .15 * M.sin(t * .5)) * (1 + .5 * crit);
    if (cs > 1) d.push(quad(1, '0132', 0, cs * .33), quad(1, '4576', 0, cs * .33), quad(3, '0415', 0, cs * .33),
      quad(3, '2637', 0, cs * .33), quad(2, '0143', 1, cs * .55), quad(2, '0253', 1, cs * .55), quad(2, '1245', 1, cs * .55));
    // Rays from frame 64; stars stretch outwards in the dolly zoom.
    const nr = rnd(k.r * smooth(ramp(t, 64, 96)) * (1 - gone));
    for (let j = 0; j < k.d && nr; ++j)
      d.push([4, [120, 67, cs * .4, 30 + 120 * crit, t * .07 + j * M.PI / nr, nr, 2 * M.PI / nr, fl ? OR : WH]]);
    const g = 1 + .8 * dz, nn = rnd(k.n * crit ** 1.5 * (1 - gone));
    d.push([5, [190 * g, 0, 0, 70 * g, t * .05, 120, 67]]);
    if (nn) d.push([6, [t * 1.7, nn, 10 + 40 * crit]]);
    return {b: fl ? 0x5acc : 0, s: 0, d: d};
  }
  const A2 = {
    names: ['TWIST', 'ZENITH', 'CORE', 'LOST'], len: [128, 96, 112, 64],
    // Plan list of scene i: [template, args...]; sh carries points.
    plans: function (i, tier) {
      const k = KN[tier], z = k.z;
      if (!i) return corrList(k.c).concat([['st', z.st]]);
      if (i === 2) return [['tri'], ['qd', GR], ['qd', OR], ['sg', GR], ['ry'], ['st', z.st], ['nz', k.k.n]];
      if (i === 3) return [['ln', WH], ['ln', BL]];
      const p = [['st', z.st], ['gr'], ['la', z.ln], ['ci', z.cr, z.cc, 3.2 / z.cc], ['sa', z.sa],
        ['tr', z.sa, z.ts]];
      for (let b = 0; b < 8; ++b) p.push(['sh', b]);
      return p.concat([['sc', z.rr], ['sw', z.sa]]);
    },
    // {b: backdrop, s: surface, d: [[plan, inputs]...]}
    frame: function (i, tier, t) {
      const k = KN[tier], z = k.z;
      if (!i) {
        // The wire opens: stars burst from the vanishing point while the
        // corridor thins to its frames and arches, then to nothing; at the cut
        // the field is ZENITH's own (g = 1).
        const e = (t - 88) / 40, g = e * e;
        const d = corrDraws(k.c, t, 128, 1 / 6).filter(q => t < 108 || t < 120 && q[0] % 2 === 0);
        if (e > 0) d.push([7, [190 * g, 0, 0, 70 * g, (t - 128) * .05, 120, 67]]);
        return {b: 0, s: 0, d: d};
      }
      if (i === 2) return core(k.k, t);
      if (i === 3) {
        // CRT off: the line shrinks to a dot, then the dot goes.
        const w = t < 10 ? 2 + 118 * (1 - t / 10) ** 2 : 2.5 - (t - 10) / 4;
        return {b: 0, s: 0, d: w > 0 ? [[0, [120 - w, 67, 120 + w, 67]],
          [1, [120 - w * .8, 66, 120 + w * .8, 66]], [1, [120 - w * .8, 68, 120 + w * .8, 68]]] : []};
      }
      const f = flight(t), c = f.c, s = f.s, H = f.H;
      const x = 34 + 24 * M.sin(t * .09) - 120, y = 28 + 10 * M.cos(t * .05) - 67;
      const P = [120 + x * c - y * s, 67 + x * s + y * c], st = [46 * c - 6 * s, 46 * s + 6 * c];
      if ((t & 3) === 3) return {b: 0, s: 1, d: [[14, []], [15,
        [120 + 60 * M.cos(t * .3), 67 + 60 * M.sin(t * .3), 120 + (P[0] - 120) * .4, 67 + (P[1] - 67) * .4,
          st[0] * .4, st[1] * .4]]]};
      const u = [c, s], b = M.max(0, M.min(7, rnd(-M.atan2(s, c) / .14 + 3.5)));
      const d = [[0, [190 * c, 190 * s, -70 * s, 70 * c, t * .05, 120, 67]],
        [1, H.concat([260 * c, 260 * s, 2.2 * M.pow(1.32, (t * .25) % 1), z.gl])],
        [2, H.concat(u, [-(z.ln - 1) * .11 - ((t * .04) % 1) * .22, 150])],
        [3, H.concat(u, [4.5, M.min(1, t / 70) * 2.2])],
        [4, P.concat(st, [10 * M.cos(t * .3), 10 * M.sin(t * .3), .35, 10])]];
      if (t % 16 < 5) d.push([5, [120, 100].concat(P, st, [20, -40])]);
      d.push([6 + b, [120, 120, 120, 123 + (t % 3) * 2]]);
      return {b: 0, s: 0, d: d};
    }
  };

  // LEN: distinct frames; PLAY: frames on screen (each news beat twice).
  const NAMES = ['NEWS', 'NEWS', 'NEWS'].concat(A2.names), LEN = [16, 16, 16].concat(A2.len),
    PLAY = LEN.map((n, s) => s < 3 ? 2 * n : n);
  // One thunk per plan: the loader builds one program array at a time.
  function specs(s, tier) {
    return (s > 2 ? A2.plans(s - 3, tier) : [0, 1, 2, 3, 4]).map(e => () => s < 3 ?
      [program(s, e), pointBatch(s, e)] :
      [prog(T[e[0]], e.slice(1)), e[0] === 'sh' ? points(e[1]) : null]);
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
    // The settled monitor: a registered image-to-image resize (bilinear).
    // ZENITH's radar thumbnail and munching squares went for the story's
    // heap (typed arrays alone keep ~1.3 KB): docs/apps/megademo-story.md
    const monRes = G.resource(G.registerResizeSource({source: res0, width: 112, height: 63}));
    const pets = V.resource('pets');
    const lives = V.cache.create([[0, 2, 9, 7], [0, 0, 2, 2], [7, 0, 9, 2]].map(b => ({bounds: b, color: 0x14f53cff})));
    const FULL = [0, 0, 240, 135], MON = [64, 20, 176, 83], RAD = [4, 84, 52, 132], ZOOM = 44;
    // Loader: the next scene registers from LOAD[0] frames before the switch,
    // LOAD[1] plans a frame, while the internal heap has LOAD[2] bytes free
    // (a registration dips it by up to 14 KB on the device); the rest
    // registers over the scene's first frames, which skip unloaded plans.
    const LOAD = [16, 1, 22528], MEM = pocket.memory;
    // The room (built only while the camera is off the screen) and the news
    // HUD: "x0 y0 x1 y1 rrggbbaa radius|-stroke|opacity" (a text when 5 has
    // letters). Window, moon, stars, desk, monitor, keyboard, cushion. One
    // literal each: a '+' keeps the pieces and the joined copy.
    const ROOM = '10 14 46 58 2a2440ff,12 16 44 56 0a1230ff,33 20 40 27 f0e8c8ff 3,31 19 38 26 0a1230ff 3,\
16 22 17 23 ffffffff,22 41 23 42 ffffffff,39 46 40 47 ffffffff,27 16 29 56 2a2440ff,12 35 44 37 2a2440ff,\
0 100 240 135 4a3020ff,0 100 240 102 6a4830ff,104 94 136 101 b0a488ff,52 10 188 96 d8ccb0ff 8,\
60 16 180 87 6a6050ff -2,150 89 176 92 6a6050ff,70 104 170 113 c8bca0ff 2,74 106 166 111 a89c80ff,\
192 99 232 105 8a3050ff 3';
    const TV = '0 110 240 135 061521ff 0 210,0 110 240 112 45cddaff,6 5 32 16 d93436ff 2,\
9 6 31 15 ffffffff LIVE,36 6 120 15 d7f4ffff POCKET_NEWS';
    // Chapters: title, room glow, card ink. Scene -> chapter is CH.
    const CH = [0, 0, 0, 1, 2, 3, 4], TITLE = 'I ON AIR|II THE LINE|III ZENITH|IV CORE|V SIGNAL LOST',
      GLOW = [0x1e3a62ff, 0x3a1856ff, 0x12402aff, 0x58301aff, 0x101428ff],
      INK = [0, 0xf848ffff, 0x18f038ff, 0xff6838ff];
    // Narration per scene: "bounds|first frame|line|..."; news headlines.
    const SAY = [, , , '6 122 234 133|10|THE CAT FALLS DOWN THE WIRE|56|IT TWISTS... AND STRAIGHTENS|\
92|AND OPENS ONTO THE STARS', '150 121 236 132|10|PILOT: THE CAT|40|BARREL ROLL!|72|SOURCE LOCKED',
    '6 106 234 117|10|THE SOURCE OF THE SIGNAL|56|THE SIGNAL OPENS ITS EYE|84|SIGNAL CRITICAL',
    '94 62 160 73|20|NO SIGNAL'];
    const NEWS = 'BREAKING: A SIGNAL FROM ORBIT|IT REPEATS EVERY NIGHT AT MIDNIGHT|\
SOURCE: ZENITH, THE UNFINISHED CITY|EXPERTS SAY IT IS LOOKING FOR A PILOT|\
IT WANTS A PILOT. IT WANTS YOUR CAT|KEEP YOUR PETS AWAY FROM THE SCREEN|1T WANTS A P1L0T. 1T WANTS Y0UR C4T';
    const HOME = [196, 70, 228, 102], NEAR = [150, 72, 182, 104];
    let tier = KN.length - 1, scene = 0, t = 0, tick = 0, live = [], pend = [], next = null, loaded = 0;
    let img, mon, hud = [], hw = [], R = {}, anim = null, built = -1, vis = false, shown = true, said = -1;
    let zoom = ZOOM, target = ZOOM, spd = 2, pin = false, fixed = false, held = 0;
    const drop = h => { for (let i = 0; i < h.length; ++i) H.unregister(h[i]); };
    const reg = th => { const p = th(); ++loaded; return p[1] ? H.register(p[0], p[1]) : H.register(p[0]); };
    const lerp4 = (a, b, e) => a.map((v, j) => rnd(v + (b[j] - v) * e)), clamp = v => M.max(0, M.min(1, v));
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
      to: {bounds: RAD, rotation: 360 * KN[tier].q}, durationMs: 4000,
      easing: 'linear', repeat: 'loop'});
    const boundsAt = step => step === 0 ? FULL : step === ZOOM ? MON :
      lerp4(FULL, MON, step / ZOOM * step / ZOOM * (3 - 2 * step / ZOOM));
    function build(tx, room) {
      hud = []; hw = []; R = {}; anim = null; vis = false; shown = true; said = -1;
      // A HUD ref shows on the full screen while from <= t < to.
      const add = (r, from, to) => { hud.push(r); hw.push(from || 0, to || 1e9, 1); return r; };
      const text = (b, s, f, c, n) => tx.text({bounds: b, text: s, font: f, color: c, capacity: n});
      const box = s => {
        const e = s.split(' '), b = e.slice(0, 4).map(Number), c = parseInt(e[4], 16), r = +e[5] || 0;
        return e[5] > 'A' ? text(b, e[5].replace('_', ' '), 'caption', c) :
          r > 0 ? tx.roundRect({bounds: b, radius: r, color: c}) : r < 0 ?
            tx.strokeRect({bounds: b, width: -r, color: c}) : tx.rect({bounds: b, color: c, opacity: +e[6] || 255});
      };
      const ch = CH[scene], S = R.S = SAY[scene] && SAY[scene].split('|'), T = TITLE.split('|');
      tx.background(255);
      if (room) {
        tx.gradient({bounds: [0, 0, 240, 100], axis: 'y', from: 0x0a0618ff, to: GLOW[ch], dither: true});
        ROOM.split(',').forEach(box);
        R.led = tx.roundRect({bounds: [62, 89, 66, 92], radius: 1, color: 0x17a7ffff});
        if (!ch || ch > 3) R.pet = tx.image({resource: pets, bounds: HOME, variant: 3, frame: 1});
        text([6, 116, 234, 125], 'NIGHT SIGNAL   ' + T[ch], 'caption', 0xd8ccb0ff);
        text([6, 125, 234, 134], 'ENTER ZOOM  L/R CHAPTER  U/D LOAD', 'caption', 0x8a7c66ff);
      }
      // The opaque image is last: at full size it exactly covers the set.
      img = tx.image({resource: res0, bounds: boundsAt(zoom), clip: FULL, sourceWidth: 240, sourceHeight: 135});
      mon = tx.image({resource: monRes, bounds: MON, clip: FULL, sourceWidth: 112, sourceHeight: 63});
      mon.setVisible(tx, false);
      if (room && ch > 3) {
        R.zz = text([206, 56, 238, 68], 'z Z z', 'caption', 0xb0c8ffff);
        R.off = text([96, 46, 150, 58], 'NO SIGNAL', 'caption', 0xffffffff);
      }
      if (scene < 3) {
        const N = NEWS.split('|');
        TV.split(',').forEach(e => add(box(e)));
        R.head = add(text([6, 114, 234, 124], N[2 * scene], 'caption', 0xffffffff, 40));
        add(text([6, 124, 234, 134], N[2 * scene + 1], 'caption', 0x80d9e8ff));
        R.N = N;
      }
      if (S) R.say = add(text(S[0].split(' ').map(Number), '', 'caption', 0xffffffff, 40), +S[1]);
      if (ch < 4) R.card = add(text([8, 36, 232, 56], R.T = T[ch], 'display', INK[ch], 16), 4, 60);
      if (scene > 3 && ch < 4) {
        R.score = add(text([4, 2, 150, 13], NAMES[scene], 'caption', 0xffffffff, 20));
        R.lamp = add(tx.roundRect({bounds: [226, 3, 236, 11], radius: 3, color: 0xfb47ffff}));
        if (scene === 4) {
          R.radar = add(tx.image({resource: res1, bounds: RAD, clip: FULL, sourceX: 60, sourceY: 7,
            sourceWidth: 120, sourceHeight: 120}));
          R.ring = add(tx.strokeRect({bounds: [3, 83, 53, 133], width: 1, color: 0x17a7ffff}));
          anim = spin(tx);
          // The pilot: the cat from the room, in the cockpit.
          add(tx.strokeRect({bounds: [3, 15, 22, 34], width: 1, color: 0x17a7ffff}));
          add(tx.image({resource: pets, bounds: [4, 16, 21, 33], variant: 3, frame: 0}));
          R.lives = [0, 1, 2].map(i => add(tx.instantiate(lives, {offset: [160 + i * 12, 4]}), 0, i > 1 ? 48 : 1e9));
        } else {
          R.plate = add(tx.rect({bounds: [60, 118, 236, 133], color: 255}));
          add(tx.rect({bounds: [62, 120, 234, 131], color: 0xfa3fffff, opacity: 90}));
          tx.group(R.plate, 2, 170);
          R.meter = add(tx.gradient({bounds: [64, 128, 65, 130], axis: 'x', from: 0x17a7ffff, to: 0xfb47ffff}));
          R.stat = add(text([64, 119, 234, 128], 'SIGNAL', 'caption', 0xffffffff, 40));
          // Interference: bands of the image, torn sideways (shown by patch).
          R.tear = []; R.tv = [];
          for (let i = 0; i < 3; ++i) {
            R.tear.push(tx.image({resource: res0, bounds: FULL, clip: [0, 0, 240, 1], sourceWidth: 240, sourceHeight: 135}));
            R.tear[i].setVisible(tx, false);
          }
        }
      }
      // Shutters last: the set turns on and off from its middle line, HUD too.
      R.top = tx.rect({bounds: MON, clip: FULL, color: 255});
      R.bot = tx.rect({bounds: MON, clip: FULL, color: 255});
      R.top.setVisible(tx, false);
      R.bot.setVisible(tx, false);
      R.cv = 1;
    }
    function patch(tx, f) {
      const on = zoom === 0, fix = zoom === ZOOM, B = boundsAt(zoom), S = R.S;
      // Interference in the last news beat, full screen only.
      const j = on && scene === 2 && t >= 16 && (t * 7) % 5 < 2 ? (t * 3) % 7 - 3 : 0;
      img.setRect(tx, [B[0] + j, B[1], B[2] + j, B[3]]);
      if (fix !== vis) { img.setVisible(tx, !fix); mon.setVisible(tx, fix); vis = fix; }
      // Shutters: open over the first news frames, close at the end of CORE.
      const o = scene === 0 ? clamp((t - 2) / 8) : scene === 5 ? clamp((110 - t) / 8) : 1;
      if (o < 1 || R.cv < 1) {
        const m = (B[1] + B[3]) / 2, h = (B[3] - B[1]) * o / 2;
        R.top.setRect(tx, [B[0], B[1], B[2], rnd(m - h)]);
        R.bot.setRect(tx, [B[0], rnd(m + h), B[2], B[3]]);
        if ((o < 1) !== (R.cv < 1)) { R.top.setVisible(tx, o < 1); R.bot.setVisible(tx, o < 1); }
        R.cv = o;
      }
      if (R.say) {
        let k = -1;
        for (let i = 1; i < S.length; i += 2) if (t >= +S[i]) k = i;
        if (k !== said && k > 0) R.say.setText(tx, S[k + 1]);
        said = k;
        if (k > 0) R.say.setReveal(tx, M.min(S[k + 1].length, t - S[k]));
      }
      for (let i = 0; i < hud.length; ++i) {
        const v = on && t >= hw[3 * i] && t < hw[3 * i + 1] ? 1 : 0;
        if (v !== hw[3 * i + 2]) { hud[i].setVisible(tx, v > 0); hw[3 * i + 2] = v; }
      }
      if (on !== shown) {
        if (anim) { if (on) anim = spin(tx); else anim.finish(tx); }
        shown = on;
      }
      // The cat: asleep, awake, into the screen (news), away, home asleep.
      if (R.pet) {
        R.pet.setRect(tx, scene === 2 && t >= 16 ? t < 24 ? lerp4(HOME, NEAR, (t - 16) / 8) :
          lerp4(NEAR, [117, 49, 123, 55], (t - 24) / 7) : HOME);
        R.pet.setImageFrame(tx, 3, scene > 5 || !scene && t < 12 || t % 40 > 37 ? 1 : 0);
        R.pet.setVisible(tx, scene !== 2 || t < 31);
      }
      if (R.zz) {
        R.zz.setReveal(tx, fix ? (t >> 3) % 6 : 0);
        R.off.setVisible(tx, fix && t >= 20 && (t & 8) > 0);
        R.led.setColor(tx, t & 16 ? 0xfb47ffff : 0x401010ff);
      }
      if (R.card) R.card.setReveal(tx, M.max(0, M.min(R.T.length, (t - 4) >> 1)));
      if (scene < 3) {
        if (scene === 2 && t >= 16) R.head.setText(tx, R.N[t & 4 ? 4 : 6]);
        return;
      }
      if (scene === 3) {
        const e = M.min(1, t / 20), w = 20 + 100 * e, h = 12 + 56 * e;
        img.setClip(tx, [rnd(120 - w), rnd(67 - h), rnd(120 + w), rnd(68 + h)]);
        return;
      }
      if (scene > 5) return;
      if (!(t & 7)) R.score.setText(tx, NAMES[scene] + ' ' + String(1e6 + tick * 10).slice(1));
      R.lamp.setColor(tx, t & (t > 71 ? 2 : 8) ? 0xfb47ffff : 0x167fffff);
      if (scene === 4) {
        for (let i = 0; i < 3; ++i) R.lives[i].place(tx, {offset: [160 + i * 12, 4 + ((t + i * 3) >> 2) % 2]});
        R.ring.setColor(tx, t > 71 ? 0xfb47ffff : 0x17a7ffff);
      } else {
        const n = f ? f.d.length : 0, p = M.min(100, t);
        R.meter.setRect(tx, [64, 128, 65 + rnd(1.69 * p), 130]);
        R.stat.setText(tx, 'SIGNAL ' + p + ' PLANS ' + live.length + ' DRAWS ' + n);
        // Tear: each band shows when a per-frame hash is under the eased
        // interference level, at a hashed height and offset.
        const nz = on ? ramp(t, 60, 100) ** 1.5 * (1 - ramp(t, 100, 104)) : 0;
        for (let i = 0; i < 3; ++i) {
          const r = R.tear[i], h = M.sin(t * 12.9898 + i * 78.233) * 43758.5453, u = h - M.floor(h), v = u < nz;
          if (v) {
            const y = rnd(u / nz * 124), dx = rnd(((u * 7) % 1 - .5) * 36 * nz);
            r.setRect(tx, [dx, 0, 240 + dx, 135]);
            r.setClip(tx, [0, y, 240, y + 3 + i * 4]);
          }
          if (v !== !!R.tv[i]) { r.setVisible(tx, v); R.tv[i] = v; }
        }
      }
      if (t === 90 && anim && on) { anim.stop(tx); console.log('MEGADEMO ANIM ' + anim.poll()); }
    }
    enter(0);
    while (pend.length) live.push(reg(pend.shift()));
    globalThis.frame = function (buttons) {
      buttons |= 0;
      const press = buttons & ~held;
      held = buttons;
      // Enter: full screen <-> the monitor in the room. The story's own
      // camera (into the screen in I, out in V) never overrides a pinned room.
      if (press & 0x4000) { target = target ? 0 : ZOOM; pin = target > 0; spd = 1; }
      if (!scene && t === 14 && !pin) { target = 0; spd = 2; }
      if (scene === 6 && t === 12 && !target) { target = ZOOM; spd = 2; }
      zoom += M.max(-spd, M.min(spd, target - zoom));
      let f = null;
      try {
        // UP/DOWN: load tier. LEFT/RIGHT: previous/next scene.
        const k = tier + (press & 0x10 ? 1 : press & 0x40 ? -1 : 0);
        if (k !== tier && k >= 0 && k < KN.length) { tier = k; enter(scene, 1); }
        if (press & 0xa0) enter((scene + (press & 0x20 ? 1 : NAMES.length - 1)) % NAMES.length);
        else if (t >= PLAY[scene]) enter((scene + 1) % NAMES.length);
        // A plan decodes in ~13 ms on the device: one a frame, this scene's
        // first, then the next scene's over the last frames. Before the
        // frame: a park here must not show a new image under the old view.
        let b = LOAD[1];
        for (; b > 0 && pend.length; --b) live.push(reg(pend.shift()));
        if (b && t >= PLAY[scene] - LOAD[0]) prefetch((scene + 1) % NAMES.length, b);
        f = frameOf(scene, tier, t % LEN[scene]);
        if (f.s) H.beginFrame(f.b, surf1);
        else H.beginFrame(f.b);
        for (let i = 0; i < f.d.length; ++i) {
          const h = live[f.d[i][0]];
          if (h) H.draw(h, f.d[i][1]);
        }
        H.commit();
      } catch (e) {
        if (!tier) throw e;
        --tier;
        console.log('MEGADEMO DEGRADE tier=' + tier + ' ' + e);
        enter(scene, 1);
        f = null;
      }
      // A set per scene and tier, rebuilt when the room comes into view.
      const room = zoom > 0, key = scene + 8 * tier + 64 * room;
      if (built !== key) {
        // Same tx: a scene's first frame must not show build()'s defaults.
        V.replace(tx => (build(tx, room), patch(tx, f)));
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
