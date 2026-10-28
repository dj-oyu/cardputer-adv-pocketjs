// MEGADEMO chunk 1 (proc_megademo.js loads it): Act I's programs and inputs
// (the news; equal to tools/kasane_contract/proc_megademo.h), the load tiers
// and the plan text decoder with Act II's templates.
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
// k: CORE (r rays a draw, d ray draws, n noise lines). Derivation:
// docs/kasane/megademo-limit-scenes.md, megademo-device-limits.md,
// docs/apps/megademo-story.md (CORE).
// TWIST and ZENITH hold the display rate at HEAVY's load, so MID shares it.
const CH = {n: 26, r: .8, m: 3, s: 8, kf: 2, ka: 3, D: 6},
  ZH = {st: 60, gl: 14, ln: 15, cr: 3, cc: 8, sa: 4, ts: 16, rr: 4};
const KN = [
  {c: {n: 12, r: .7, m: 2, s: 4, kf: 2, ka: 3, D: 4}, z: {st: 24, gl: 8, ln: 7, cr: 2, cc: 4, sa: 2, ts: 6, rr: 2},
   k: {r: 20, d: 1, n: 12}},
  {c: CH, z: ZH, k: {r: 40, d: 1, n: 24}},
  {c: CH, z: ZH, k: {r: 40, d: 2, n: 40}}
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
// 12 edges), tr CUBIC trails, sh ship flame, rd/sw radar rings and sweep
// (drawn in the image's corner: a second surface cost two 10 KB frames), ln a line; CORE:
// tri a triangle (colour from input 6), qd a closed quad, sg two segments,
// ry rays round a centre (REPEAT_REG), nz scan noise (BREAK_IF_GT); WAKE:
// tz a filled trapezoid (input 5 rows of horizontal lines, x0..x1 at y,
// stepping by inputs 3 and 4): a path filled like a bucket fill.
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
  tz: 'U6 S6,1 Q5 V0,2 L1,2,$0 A0,0,3 A1,1,4 A2,2,6 E',
  ry: 'U8 S8,1.5707963 Q5 A9,4,8 N9,9 N10,4 M11,9,2 A11,11,0 M12,10,2 A12,12,1 V11,12 M11,9,3 A11,11,0 M12,10,3 A12,12,1 l7,11,12 A4,4,6 E',
  nz: 'U3 S3,0 S4,1 S5,67 S6,120 S7,7.31 S8,3.97 R$0 A3,3,4 B3,1 M9,3,7 A9,9,0 N9,9 M9,9,5 A9,9,5 M10,3,8 A10,10,0 N10,10 M10,10,6 A10,10,6 V10,9 A11,10,2 L11,9,33808 E',
  rd: 'U3 S4,1.5707963 S5,.3926991 S6,1.45 R$0 S3,0 A8,3,4 N8,8 M8,8,2 A8,8,0 N9,3 M9,9,2 A9,9,1 V8,9 R16 A3,3,5 A8,3,4 N8,8 M8,8,2 A8,8,0 N9,3 M9,9,2 A9,9,1 L8,9,6055 E M2,2,6 E S10,-1 M11,2,10 A12,11,0 A13,2,0 V12,1 L13,1,6055 A12,11,1 A13,2,1 V0,12 L0,13,6055',
  sw: 'S8,$1 S9,$2 V8,9 I0,0 I1,1 L0,1,65535 I2,2 I3,3 I4,4 I5,5 S10,65535 S11,129862 S12,-1 R$0 p10,2,3 A2,2,4 A3,3,5 M10,10,12 A10,10,11 E',
};
