// MEGADEMO's plan templates and their decoder. Not an app chunk: the
// firmware build compiles every template into its built-in plan table
// (tools/kasane_ir/emit_rom_plans.mjs, `kasane.<name>`; chunks.txt's
// rom-text line), and the app registers plans by that name, so neither
// the text nor the decoder is in the guest heap. The host tests evaluate
// this file after megademo_act1.js (hx) to read the rows
// (tools/kasane_contract/megademo_host.js). docs/apps/megademo-story.md.
'use strict';
// Programs are text, decoded by one function. A letter per ksn_proc_op in
// opcode order, then its fields: f lists the row slot of each, a digit
// for a register (one hex character), a letter for a number (b = 2, d = 4,
// e = 5; comma-separated, $n is an argument). X x y r zi zn t0 t1 z0 is
// (x, y) *= z in place (registers; z0 reads zero); Un[o] loads n inputs
// from register o.
function prog(S, A) {
  const c = [];
  for (let p = 0, q = 0; p < S.length; p = q + 1) {
    q = S.indexOf(' ', p);
    if (q < 0) q = S.length;
    const o = S[p];
    if (o === 'U') {
      for (let j = 0, n = hx(S, p + 1), b = q > p + 2 ? hx(S, p + 2) : 0; j < n; ++j) c.push([1, b + j, j, 0, 0, 0]);
    } else if (o === 'X') {
      const x = hx(S, p + 1), y = hx(S, p + 2), r = hx(S, p + 3), a = hx(S, p + 6), b = hx(S, p + 7);
      c.push([3, a, x, r, 0, 0], [3, b, y, hx(S, p + 5), 0, 0], [2, a, a, b, 0, 0], [3, b, x, hx(S, p + 4), 0, 0],
        [3, y, y, r, 0, 0], [2, y, y, b, 0, 0], [2, x, a, hx(S, p + 8), 0, 0]);
    } else {
      const k = 'SIAMNREVPLQBplC'.indexOf(o), R = [k, 0, 0, 0, 0, 0],
        f = ['1d', '12', '123', '123', '12', 'b', '', '23', '23e', '23e', '2', '23', '123', '123', 'be'][k];
      let i = p + 1, m = null;
      for (let j = 0; j < f.length; ++j) {
        const r = f.charCodeAt(j);
        if (r < 58) R[r - 48] = hx(S, i++);
        else {
          if (!m) m = S.slice(i, q).split(',');
          const x = m.shift();
          R[r - 96] = x[0] === '$' ? A[x[1]] : +x;
        }
      }
      R[4] = M.fround(R[4]);
      c.push(R);
    }
  }
  if (c.length > 64) throw RangeError('64 instructions');
  return c;
}
const T = {
  // Act I's HUD (megademo_brief.js). cv/ch: n ticks of length L across
  // the line at, from p0 every sp (inputs at, L, p0, sp, n), drawn across
  // or down. eg: a brain wave (y0, amplitude, sample, rhythm, noise, colour):
  // two sines, the second chirped, and sin^16 a spike every 32 samples.
  // cw: the carrier (y0, amplitude, phase, depth and phase of its
  // modulation). sp: a spectrum's 15 bars, one skyline (noise phase,
  // gain). sc: a Lissajous 3:2 (phase).
  cv: 'U5 A501 Q4 V02 L52$0 A223 E',
  ch: 'U5 A501 Q4 V20 L25$0 A223 E',
  eg: 'U6 S66 S72.4 S81 S9.0981748 R48 Ma23 Naa Mb22 Mbb4 Nbb Aaab Mb29 Nbb Mbbb Mbbb Mbbb Mbbb Aaab Aaab Maa1 \
Aaa0 l56a A667 A228 E',
  cw: 'U5 S50 S65 S7.55 S8.13 S90 Sa1 R49 Mb97 Abb2 Nbb Mc98 Acc4 Ncc Mcc3 Acca Mbbc Mbb1 Abb0 L5b$0 A556 A99a \
E',
  sp: 'U2 S2128 S3131 S428.6 S5-1.3 S60 S77.31 S83 S91 Sa-1 R15 Ab60 Nbb Mbbb Mbb4 Mbb1 Abb9 Abb9 Mbba Abb3 \
L23$0 L2b$0 A228 L2b$0 L23$0 A229 A667 A445 E',
  sc: 'U1 S10 S2.4712 S30 S4.3142 S5214 S618 S7119 S811 R41 A910 N99 M996 A995 Na3 Maa8 Aaa7 L9a$0 A112 A334 E',
  // TWIST: fr frames (16 registers), ra two polylines, ru rungs, ar CUBIC
  // arches. ZENITH: st stars, gr ground, la lanes, ci skyline, sa octahedra
  // (one path over 12 edges), tr CUBIC trails, sh ship flame, rd/sw radar
  // rings and sweep (in the image's corner), ln a line. CORE: tri a
  // triangle (colour from input 6), qd a closed quad, sg two segments, ry
  // rays round a centre (REPEAT_REG), nz scan noise (BREAK_IF_GT).
  fr: 'U6 I86 I97 Sd-1 Ma9d Me2d A604 A66e Me3d A715 A77e Sb120 Sc67 R$0 Adb0 Aec1 Vde Adb2 Aec3 Lde$1 Adb4 \
Aec5 Lde$1 Adb6 Aec7 Lde$1 Adb0 Aec1 Lde$1 X0189adef X2389adef X4589adef X6789adef E',
  ra: 'U6 S9-1 M659 S7120 S867 R$0 A970 Aa81 V9a X014569ab A970 Aa81 L9a$1 A972 Aa83 V9a X234569ab A972 Aa83 \
L9a$1 E',
  ru: 'U6 Se-1 M65e S7120 S867 Sc$1 Sd$2 R$0 A970 Aa81 V9a A972 Aa83 lc9a Mcce Accd X014569ab X234569ab E',
  ar: 'U88 R$0 S4120 S567 A048 A159 A64a A75b A20c A31d A46c A57d C$1,$2 S3-1 M2f3 S30 X89ef2013 Xabef2013 \
Xcdef2013 E',
  st: 'U7 S81 S91.618034 Se.6 Sf-.4 R$0 A778 Aa74 Naa Mb79 Nbb Mbbe Abbf Mca0 Acc5 Mdb2 Accd Mda1 Add6 Mab3 \
Adda Pcd65535 E',
  gr: 'U8 S81.32 S9-1 Se6055 Sf70382 Q7 Ma26 Aaa0 Mb36 Abb1 Aca4 Adb5 Vcd Mc49 Acca Md59 Addb lecd Mee9 Aeef \
M668 E',
  la: 'U8 S8.22 S91.5 R$0 Mb46 Abb2 Mc56 Acc3 Mdb9 Add0 Mec9 Aee1 Vde Mdb7 Add0 Mec7 Aee1 Lde5759 A668 E',
  ci: 'U8 Sd$2 Se1.6 Sf-1 R$0 S8-1.6 Mb84 Abb2 M9b6 A990 Mb85 Abb3 Mab6 Aaa1 V9a R$1 Mc86 Ncc Mccc Mcc7 Mcc6 \
Mccf Mb84 Abb2 M9b6 A990 Mb85 Abb3 Mab6 Aaa1 Mbc2 A99b Mbc3 Aaab L9a64063 A88d Mb84 Abb2 M9b6 A990 Mb85 \
Abb3 Mab6 Aaa1 Mbc2 A99b Mbc3 Aaab L9a64063 E M66e E',
  sa: 'U8 Sd-1 Sa1.3 M87a M98d Mc65 M764 M65d Me4d Mfcd R$0 Aa19 V0a Aa04 Ab1c Lab65535 Aa18 L0a64063 Aa06 Ab17 \
Lab65535 Aa19 L0a64063 Aa0e Ab1f Lab65535 Aa18 L0a64063 Mb6d Aa0b Mb7d Ab1b Lab65535 Aa04 Ab1c Lab65535 \
Aa06 Ab17 Lab65535 Aa0e Ab1f Lab65535 Mb6d Aa0b Mb7d Ab1b Lab65535 Aa19 L0a64063 A002 A113 E',
  tr: 'U88 R$0 S00 A008 S10 A119 S20 A228 A39f S40 A44a A5be S60 A66a S70 A77b C$1,64327 Aaac Abbd E',
  sh: 'U4 V01 L2364327',
  ln: 'U4 V01 L23$0',
  tri: 'U7 V01 l623 l645 l601',
  qd: 'U8 V01 L23$0 L45$0 L67$0 L01$0',
  sg: 'U8 V01 L23$0 V45 L67$0',
  // ht: dot tone in a triangle: n rows (6) from the apex (0, 1) down one
  // edge (2, 3), row i i * k dots (k: 7) a step (4, 5) apart; the colour
// from a register (PLOT_COLOR_REG, which the radar's sweep used).
  ht: 'U8 Sd1 Se0 Sf$0 Q6 A002 A113 Aee7 M90d Ma1d Qe pf9a A994 Aaa5 E E',
  ry: 'U8 S81.5707963 Q5 A948 N99 Na4 Mb92 Abb0 Mca2 Acc1 Vbc Mb93 Abb0 Mca3 Acc1 l7bc A446 E',
  nz: 'U3 S30 S41 S567 S6120 S77.31 S83.97 R$0 A334 B31 M937 A990 N99 M995 A995 Ma38 Aaa0 Naa Maa6 Aaa6 Va9 \
Aba2 Lb933808 E'
};
