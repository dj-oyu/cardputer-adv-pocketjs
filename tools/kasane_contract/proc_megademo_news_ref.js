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
const ACT1 = ['U2 S2$0 S3$1 S4$2 S5$3 S6$4 S7-7 R$5 V02 L12$6 L13$6 L03$6 L02$6 A004 A115 A226 A337 E',
  'U2 S2$0 S3134 S423 S54.8 R12 V12 L03$1 A004 A115 E',
  'S00 S1239 I20 S31.5 S41.47 R10 V02 L12$0 A223 M334 E',
  'S00 S14 S2$0 I30 S4$1 S5$2 R60 M602 A663 N66 M764 A775 L07$3 A001 E',
  'I00 S129 S20 S3239 I41 S62 R4 V20 L30$2 A506 V25 L35$0 A001 A224 A334 E S088 S1152 S232 S398 S4120 \
S565 I62 I73 V42 L15$2 L43$2 L05$2 L42$2 V42 L43$3 V05 L15$3 V42 A446 A227 L42$3 V15 A116 A557 L15$3 \
S4120 V43 A446 A337 L43$3 S565 V05 A006 A557 L05$3 V42 L15$1 L43$1 L05$1 L42$1'];
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
