// The briefing as it was before pocket.shapes (Act I's VM programs and
// inputs): the reference workload of the procedural VM's contract tests,
// equal to proc_megademo.h. Evaluated after apps/kasane/megademo_act1.js
// (its prog() and f32); the app no longer draws it.
'use strict';
const colors = [
  [0x07ff, 0x3dff, 0xb81f, 0xffff],
  [0xfde0, 0xfb00, 0x07ff, 0xffff],
  [0xf81f, 0x781f, 0x07ff, 0xffff]
];
const backdrops = [0x080c, 0x000d, 0x100b];

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
function newsProgram(phase, layer) {
  const n = colors[phase], a = phase === 1;
  return prog(ACT1[layer], [
    [a ? 42 : 5, a ? 125 : 129, a ? 13 : 12, a ? -13 : -12, a ? 3 : 7, a ? 6 : 8, n[0]],
    [a ? 64 : 68, n[1]], [n[2]],
    [f32(f32(.073) + f32(phase * .015)), a ? 7 : 13, phase === 2 ? 42 : 28, n[3]], n][layer]);
}

function newsInputs(frame, layer) {
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
function newsPointBatch(phase, layer) {
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

globalThis.procMegademoNews = {
  program: newsProgram, inputs: newsInputs, pointBatch: newsPointBatch,
  backdrop: frame => backdrops[Math.floor(frame / 16)]
};
