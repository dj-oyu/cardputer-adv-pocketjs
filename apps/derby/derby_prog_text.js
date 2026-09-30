'use strict';
// ---- Programs as text, one letter per ksn_proc_op (MEGADEMO's form).
// NOT loaded by the app: tools/kasane_ir/pack.mjs packs this file into
// derby_prog.js (4-bit registers, ~1.6 KB less resident); edit here, then
// node tools/kasane_ir/pack.mjs apps/derby/derby_prog_text.js
// apps/derby/derby_prog.js --nibble (tools/games/run_derby.py checks it).
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
// [LIGHT, MID, HEAVY]: stand tiers, crowd rows, dots per bay, roof arc
// segments, rail post and turf stripe spacing (m).
const KN = [[3, 2, 3, 3, 8, 10], [4, 3, 4, 5, 5, 7], [5, 4, 6, 8, 4, 5]];
let tier = 1;
const T = {
  rail: 'I0,0 I1,1 I2,2 I3,3 I4,4 I5,5 I8,6 S6,0 A6,6,0 M7,4,1 A7,7,0 V6,2 l8,7,2 V6,5 l8,7,5 Q4 V0,2 l8,0,3 A0,0,1 E',
  turf: 'I0,0 I1,1 I2,2 I3,3 I4,4 I5,5 I6,6 I9,7 S10,288 M9,9,10 S8,512 A8,8,9 S11,-1 S12,1312 Q6 V0,4 l8,1,5 A0,0,2 A1,1,3 M8,8,11 A8,8,12 E',
  stands: 'I8,0 I9,1 I10,2 I11,3 I12,4 I7,7 S15,0 S0,0 S2,239 A1,10,15 R$0 V0,1 L2,1,21130 A1,1,11 E I1,7 A3,7,11 A3,3,11 A5,3,15 Q12 A0,8,15 A2,8,15 A4,8,9 A6,8,9 V8,10 L8,7,31727 C$3,50712 A8,8,9 E',
  crowd: 'I8,0 I9,1 I10,2 I11,3 I12,4 I14,6 S15,12650 M14,14,15 S15,46496 A14,14,15 S15,105642 S1,1.5 S2,2.39996 S4,-1 S3,$6 M3,3,9 S13,0 S7,.5 M5,11,7 A5,5,10 R$1 I0,0 I6,5 A6,6,13 Q12 R$2 N7,6 M7,7,1 A7,7,0 p14,7,5 A0,0,3 A6,6,2 M14,14,4 A14,14,15 E E A5,5,11 S7,.9 A13,13,7 E',
  runner: 'I0,0 I1,1 I2,2 I3,3 S4,2.5 M2,2,4 S4,-1 M4,4,2 A5,0,4 A6,0,2 A7,6,2 A8,7,2 A9,8,2 A10,9,2 A11,1,2 A12,1,4 A13,12,4 V5,11 L0,1,$0 L8,1,$0 L9,13,$0 L10,12,$0 L9,12,$0 I14,4 V0,1 L14,3,$0 I14,5 V0,1 L14,3,$0 I14,6 V8,1 L14,3,$0 I14,7 V8,1 L14,3,$0 V7,1 L7,12,$1 L8,13,$1 L9,12,$1 P8,13,$2',
  gate: 'I0,0 I1,1 I2,2 I3,3 I4,4 S5,120 R9 A6,5,0 A7,4,1 A8,4,2 L6,8,40147 L6,7,40147 V6,8 M0,0,3 M1,1,3 M2,2,3 E',
  pole: 'I8,0 I9,1 I10,2 I11,3 V8,9 L8,10,63488 S15,-1 M12,11,15 A13,10,12 S14,-1.33 M14,14,11 S15,0 A0,8,12 A1,13,15 A2,0,15 A3,13,14 A4,8,11 A5,3,15 A6,4,15 A7,13,15 C6,65535 S15,-1 M14,14,15 A3,13,14 A5,13,14 C6,65535 I0,4 I1,5 I2,6 I3,7 V0,1 L2,3,65535 S15,1 A0,0,15 A2,2,15 V0,1 L2,3,65535',
  photo: 'I0,0 S1,13 S2,127 V0,1 L0,2,65535 S3,1 A0,0,3 V0,1 L0,2,63488 S9,0 S10,239 V9,1 L10,1,50712 V9,2 L10,2,50712 S4,0 S5,128 S6,133 S7,6 S8,131 R8 R4 V4,5 L4,8,50712 A4,4,7 E V4,5 L4,6,65535 A4,4,7 E',
  conf: 'I2,0 I1,1 S0,0 S9,1 S8,-1 S6,65504 S7,129055 S11,1.7 S12,.037 S13,2.1 S14,.00041 R96 A0,0,9 B0,1 M5,0,11 N3,5 S5,110 M3,3,5 S5,120 A3,3,5 M5,0,14 A5,5,12 M5,5,2 M4,0,13 A4,4,5 N4,4 S5,64 M4,4,5 S5,67 A4,4,5 V3,4 A5,3,9 A10,4,9 A10,10,9 l6,5,10 M6,6,8 A6,6,7 E',
  // The jockey over the close-up horse (points baked for HX,HY; README):
  // silk colour input 0, bob (px) input 1, white cap by CUBIC.
  silk: 'I8,0 I9,1 S0,113 S1,96 A1,1,9 S2,116 S3,92 A3,3,9 S4,109 S5,84 A5,5,9 S6,119 S7,74 A7,7,9 V0,1 l8,2,3 l8,4,5 l8,6,7 S0,127 S1,80 A1,1,9 l8,0,1 S0,120 S1,71 A1,1,9 S2,120 S3,66 A3,3,9 S4,128 S5,66 A5,5,9 S6,128 S7,71 A7,7,9 C5,65535',
  map: 'S9,-4 S10,8 S11,12 S12,232 S13,2 V10,11 L12,11,16904 V12,13 L12,11,63488',
  // The screen: its face filled row by row in colour input 6 (the feed
  // is drawn over it), b bezel rings round it, a top light, two legs.
  vis: 'I0,0 I1,1 I2,2 I3,3 I4,4 I5,5 I15,6 S6,-1 S7,1 A8,0,7 A9,2,6 A10,1,7 M11,1,6 A11,11,3 A11,11,6 Q11 V8,10 l15,9,10 A10,10,7 E Q4 V0,1 L2,1,10565 L2,3,10565 L0,3,10565 L0,1,10565 A0,0,6 A1,1,6 A2,2,7 A3,3,7 E V0,1 L2,1,23275 M8,0,6 A8,8,2 S9,.25 M9,8,9 A9,9,0 S11,.5 M8,8,11 A10,9,8 M11,4,6 A12,9,11 V12,3 L12,5,19049 A12,9,4 V12,3 L12,5,19049 A12,10,11 V12,3 L12,5,19049 A12,10,4 V12,3 L12,5,19049',
  // A horse head on (HEAD ON cut): x, ground y, px per unit, coat, silk,
  // lift of each foreleg (px).
  fr: 'I0,0 I1,1 I2,2 I5,5 I6,6 I8,3 I9,4 S15,-1 M5,5,15 A5,5,1 M6,6,15 A6,6,1 S3,-1.2 M3,3,2 A3,3,0 M4,3,15 A4,4,0 A4,4,0 S7,-6 M7,7,2 A7,7,1 V3,7 l8,3,5 V4,7 l8,4,6 M14,2,15 M11,2,15 A11,11,0 A12,0,2 A10,11,14 A13,12,2 S5,-9.5 M5,5,2 A5,5,1 A6,5,2 V10,7 l8,10,6 l8,11,5 l8,12,5 l8,13,6 l8,13,7 l8,10,7 S7,-13 M7,7,2 A7,7,1 V3,5 l9,11,7 l9,12,7 l9,4,5 A7,7,14 p9,0,7 A6,6,2 A5,5,14 A5,5,14 V0,6 l8,0,5',
  // Nothing: the code of the typed-point plans (the gallop frames, hd).
  nil: 'S0,0'
};
for (let k = 0; k < 8; ++k) T.map += ' I0,' + k + ' S1,' + (3 + k) + ' V0,1 A0,0,9 L0,1,' + SILK[k];
// The close-up horse: 6 gallop frames of one polyline (typed points, half
// pixels), fixed on screen with the hip at HX,HY and 4 px per unit.
const HX = 92, HY = 88, HS = 4, bob = k => .35 * sin(2 * PI * k / 3);
// Units, y down, hip at 0,0; a pair 20+o,l is a leg of phase o and
// segment length l from the point before it (knee, hoof, back up).
const BODY = [-2.4, 3.2, -1.6, 1.4, -.6, 0, .3, -.4, 3.8, .3, 7, -.9, 8.3, -3.1, 9.9, -5, 9.8, -6, 10.5, -4.9,
  12.7, -2.3, 12.5, -1.6, 10.4, -2.4, 9.3, -2, 8.6, .8, 7.4, 2.2, 23.14, 1.9, 23.64, 1.9, 4, 2.7, 1.2, 2, 20, 2,
  20.5, 2, -.5, 1.2, -.6, 0];
let F = null;
// HEAD ON's fixed set (rails, finish line, stands, the screen edge on) as
// one polyline; points from the camera in README.
const HD = [[120, -155, 120, 425, 120, -155, -63, 323, 425, 120, 85, 159, 120, 100, 142, 120, 184, 166, 166, 166, 156, 156, 166, 166, 184, 184, 189, 189, 196, 196, 204, 204, 214, 214, 227, 227, 240, 227, 227, 240, 227, 227, 120, 184],
  [50, 105, 50, 105, 50, 160, 123, 123, 160, 50, 64, 64, 50, 58, 58, 50, 56, 56, 40, 14, 21, 42, 40, 56, 56, 19, 17, 56, 57, 14, 10, 58, 59, 5, -1, 60, 61, 60, -1, -8, -1, 60, 50, 19]];
const PAD = ['turf', 'rail', 'stands', 'silk', 'g0', 'g1', 'g2', 'g3', 'g4', 'g5', 'crowd', 'pole'],
  RUN = ['gate', 'r0', 'r1', 'r2', 'r3', 'r4', 'r5', 'r6', 'r7', 'map', 'vis', 'fr', 'hd'];
