// MEGADEMO chunk 1 (proc_megademo.js loads it): the load tiers and the plan
// text decoder with Act II's templates. (Act I draws with pocket.shapes,
// megademo_brief.js; its old VM programs are the contract tests' workload,
// tools/kasane_contract/proc_megademo_news_ref.js.)
'use strict';
const f32 = Math.fround;
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
// ry rays round a centre (REPEAT_REG), nz scan noise (BREAK_IF_GT). WAKE
// has no plans (pocket.shapes draws it).
const T = {
  // Act I's HUD (megademo_brief.js). cv/ch: n ticks of length L across
  // the line at, from p0 every sp (inputs at, L, p0, sp, n), drawn across
  // or down. eg: a brain wave (y0, amplitude, sample, rhythm, noise, colour):
  // two sines, the second chirped, and sin^16 a spike every 32 samples.
  // cw: the carrier (y0, amplitude, phase, depth and phase of its
  // modulation). sp: a spectrum's 15 bars, one skyline (noise phase,
  // gain). sc: a Lissajous 3:2 (phase).
  cv: 'U5 A5,0,1 Q4 V0,2 L5,2,$0 A2,2,3 E',
  ch: 'U5 A5,0,1 Q4 V2,0 L2,5,$0 A2,2,3 E',
  eg: 'U6 S6,6 S7,2.4 S8,1 S9,.0981748 R48 M10,2,3 N10,10 M11,2,2 M11,11,4 N11,11 A10,10,11 M11,2,9 N11,11 \
M11,11,11 M11,11,11 M11,11,11 M11,11,11 A10,10,11 A10,10,11 M10,10,1 A10,10,0 l5,6,10 A6,6,7 A2,2,8 E',
  cw: 'U5 S5,0 S6,5 S7,.55 S8,.13 S9,0 S10,1 R49 M11,9,7 A11,11,2 N11,11 M12,9,8 A12,12,4 N12,12 M12,12,3 \
A12,12,10 M11,11,12 M11,11,1 A11,11,0 L5,11,$0 A5,5,6 A9,9,10 E',
  sp: 'U2 S2,128 S3,131 S4,28.6 S5,-1.3 S6,0 S7,7.31 S8,3 S9,1 S10,-1 R15 A11,6,0 N11,11 M11,11,11 M11,11,4 \
M11,11,1 A11,11,9 A11,11,9 M11,11,10 A11,11,3 L2,3,$0 L2,11,$0 A2,2,8 L2,11,$0 L2,3,$0 A2,2,9 A6,6,7 A4,4,5 E',
  sc: 'U1 S1,0 S2,.4712 S3,0 S4,.3142 S5,214 S6,18 S7,119 S8,11 R41 A9,1,0 N9,9 M9,9,6 A9,9,5 N10,3 M10,10,8 \
A10,10,7 L9,10,$0 A1,1,2 A3,3,4 E',
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
  rd: 'U3 S4,1.5707963 S5,.3926991 S6,1.45 R$0 S3,0 A8,3,4 N8,8 M8,8,2 A8,8,0 N9,3 M9,9,2 A9,9,1 V8,9 R16 A3,3,5 A8,3,4 N8,8 M8,8,2 A8,8,0 N9,3 M9,9,2 A9,9,1 L8,9,6055 E M2,2,6 E S10,-1 M11,2,10 A12,11,0 A13,2,0 V12,1 L13,1,6055 A12,11,1 A13,2,1 V0,12 L0,13,6055',
  sw: 'S8,$1 S9,$2 V8,9 I0,0 I1,1 L0,1,65535 I2,2 I3,3 I4,4 I5,5 S10,65535 S11,129862 S12,-1 R$0 p10,2,3 A2,2,4 A3,3,5 M10,10,12 A10,10,11 E',
};
