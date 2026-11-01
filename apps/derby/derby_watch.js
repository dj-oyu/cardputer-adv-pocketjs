// DERBY WATCH: a night race on a 1000 m straight in pseudo-3D line art.
// Why and numbers: apps/derby/README.md, docs/apps/derby-watch.md
// This entry is the race model; the game (pocket.kasane) is in the
// chunks.txt scripts, loaded while this evaluates: one parse at a time.
'use strict';
const M = Math, mx = M.max, mn = M.min;
// Integers only (docs/apps/derby-integer.md): x Q16 m; speeds Q20 m a step
// (a step is the sim's 0.05 s); time Q16 steps; a runner's figures keep the
// Q16 fractions they were drawn from (qt, qs, qk: the paddock's bars).
const D = 1000, DQ = 65536000;
// Race model, tuned by tools/games/tune_derby.mjs (README has the terms,
// here in the units above): TOP 16.6 m/s, SPR .2, ST0 .8, ST1 1, KI0 .02,
// KI1 .015, PACE .015, FORM .6 m/s, WR .995, WS .02 m/s, FADE and EB .97, PK0
// .03, PK1 .5 m/s, WIND 1.5, KS .25, BG 4 m, BK .15.
const TOP = 870318, SPR = 10486, ST0 = 52429, KI0 = 1311, KI1 = 983, PACE = 983, FORM = 31457, WR = 328, WS = 1049,
  FADE = 1966, PK0 = 1573, PK1 = 26214, BG = 262144, BK = 7864, ACC = 13107, ACC1 = 5243, DV = 2621;
const POOL = 'BIT DASH,HEX HOOF,NULL MOON,BAUD STAR,LOOP KID,CACHE HIT,TIN KITE,OPCODE,PIXEL RUN,STACK TOP,BYTE MARE,MOSS DRUM'.split(',');
const SILK = [0xffff, 0x8c71, 0xf800, 0x237f, 0xffe0, 0x07e0, 0xfd20, 0xf81f], STY = ['FRONT', 'STALK', 'CLOSE'];
const z8 = v => [v, v, v, v, v, v, v, v];
// The stream's next 32 bits (int32); f16: its top 16 as a Q16 fraction.
// a + b mod 2^32 by halves: an int32 '+' that overflows is a double.
const add = (a, b) => { const l = (a & 65535) + (b & 65535); return ((a >>> 16) + (b >>> 16) + (l >>> 16) << 16) | l & 65535; };
function rng(s) {
  return function () {
    s = s < 315917835 ? s + 0x6d2b79f5 : s - 1231700742 - 1231700741; // + 0x6d2b79f5 mod 2^32
    let t = M.imul(s ^ s >>> 15, 1 | s);
    t = add(t, M.imul(t ^ t >>> 7, 61 | t)) ^ t;
    return t ^ t >>> 14;
  };
}
const f16 = r => r() >>> 16;
// floor(a / b) and floor(a 2^16 / b) (a / b < 2^15) for 0 <= a < 2^30,
// 0 < b < 2^30, by shifts (the JS '/' is a double).
function idiv(a, b) {
  let q = 0, d = b, k = 0;
  while (d <= a >> 1) d <<= 1, ++k;
  for (; k >= 0; --k, d >>= 1) { q <<= 1; if (a >= d) a -= d, ++q; }
  return q;
}
function fdiv(a, b) {
  let q = idiv(a, b);
  a -= q * b;
  for (let k = 0; k < 16; ++k) { a <<= 1; q <<= 1; if (a >= b) a -= b, q |= 1; }
  return q;
}
// The course (README "楕円"): oval with chance OV[0] (Q16). On its bend
// (273..650 m of a runner's own x) its progress is scaled by 1 + its lane's
// OL[i] (OV[1] (3.5 - i) / 3.5, .0025) and a draw (OV[2], .02) from a stream
// of its own (c), which also picks the course (Q16).
const OV = [32768, 164, 1311], OL = [164, 117, 70, 23, -23, -70, -117, -164];
function field(seed) {
  const r = rng(seed), c = rng(add(seed, 0x2545f491)), p = POOL.slice(), h = [], m = [];
  for (let i = 0, a, b; i < 8; ++i) {
    const k = {n: p.splice(f16(r) * p.length >> 16, 1)[0], qt: f16(r)};
    k.top = TOP + (SPR * k.qt >> 16); k.qs = f16(r); k.st = ST0 + k.qs; k.qk = f16(r); k.kick = KI0 + (KI1 * k.qk >> 16);
    k.sty = 3 * f16(r) >> 16; k.qr = f16(r); k.re = 5 * k.qr; k.qa = f16(r); k.acc = ACC + (ACC1 * k.qa >> 16);
    k.coat = [0xdd8c, 0xc460, 0xbdf7, 0xef5b][4 * f16(r) >> 16];
    // EB: the speed past which a runner tires, and its drain a Q20 of excess.
    k.vth = k.top - (k.top * FADE >> 16); k.kd = idiv(214748365, k.top); k.kq = k.kick * (4 + k.sty) >> 2;
    h.push(k);
    a = OL[i]; b = OV[2] * (2 * f16(c) - 65536) >> 16; m[i] = a + b + (M.imul(a, b) >> 16);
  }
  return {seed: seed, h: h, m: m, o: f16(c) < OV[0]};
}
// Arithmetic only, no Math.sin/exp: a seed replays bit for bit anywhere.
// Per race a form offset the odds cannot see, and a slow random walk.
function race(f) {
  const r = rng(add(f.seed, 0x5bd1e995)), fm = z8(0), e = [];
  for (let i = 0; i < 8; ++i) { e[i] = f.h[i].st; fm[i] = FORM * (f16(r) - 32768) >> 16; }
  return {t: 0, x: z8(0), v: z8(0), e: e, sb: z8(0), px: z8(0), tc: z8(0), fm: fm, w: z8(0), r: r, done: 0};
}
// One step: s.t += 1 step; the finish time interpolated in the step.
function step(f, s) {
  const t0 = s.t, xl = mx.apply(null, s.x);
  s.t += 65536;
  for (let i = 0; i < 8; ++i) {
    const h = f.h[i], x = s.x[i], top = h.top + s.fm[i], gap = xl - x, early = x < (500 + 80 * h.sty) * 65536;
    let v = s.v[i];
    s.px[i] = x;
    if (s.t < h.re) continue;
    // Early the field bunches and the leader pays for the wind; late, a
    // runner within BG m of the lead digs in.
    let g = s.e[i] <= 0 ? top - (top * FADE >> 16) : early ? top - (top * PACE >> 16) * h.sty + mn(PK1, (gap >> 4) * PK0 >> 12) :
      top + ((top >> 4) * h.kq >> 12);
    if (!early && gap < BG) g += gap * BK >> 16;
    g += s.w[i] = s.w[i] - (s.w[i] * WR + 32768 >> 16) + (WS * (f16(s.r) - 32768) >> 16);
    if (f16(s.r) < 39) s.sb[i] = 10;
    if (s.sb[i] > 0) s.sb[i]--, g -= (g >> 4) * 6554 >> 12;
    const a = g - v, up = h.acc;
    v += a > up ? up : a < -DV ? -DV : a;
    if (v > h.vth) s.e[i] -= (v - h.vth) * (gap < 65536 ? 3 : 2) * h.kd >> 17;
    const y = s.x[i] = x + ((s.v[i] = v) + (f.o && x >= 17891328 && x < 42598400 ? M.imul(v, f.m[i]) >> 16 : 0) >> 4);
    if (x < DQ && y >= DQ) s.tc[i] = t0 + fdiv(DQ - x, y - x), ++s.done;
  }
}
const order = s => [0, 1, 2, 3, 4, 5, 6, 7].sort((a, b) => (s.tc[a] || 1073741824) - (s.tc[b] || 1073741824) || s.x[b] - s.x[a] || a - b);
// Win chance: a Luce model of the paddock figures (weights fitted), 20% take;
// on the oval (o) a temperature (.92), the lane (OW: .15 (3.5 - i) / 3.5)
// and a take refitted (README). The weights act on the figures' fractions:
// top 3.44, st 2.6, re -.575, acc 1, a closer .5 (Q16, base e); each weight
// is 2^(exponent), EX the Q16 2^(k/16). Odds in tenths, 1.1..999.
const OW = [9830, 7022, 4213, 1404, -1404, -4213, -7022, -9830],
  EX = [65536, 68438, 71468, 74632, 77936, 81386, 84990, 88752, 92682, 96785, 101070, 105545, 110218, 115098, 120194, 125515, 131072];
function odds(h, o) {
  const u = [], p = [];
  let z = 0, i, k, w = -1000000000;
  for (i = 0; i < 8; ++i) {
    k = h[i];
    let v = (k.qt * 3523 >> 10) + (k.qs * 2662 >> 10) - (k.qr * 589 >> 10) + k.qa + (k.sty > 1 ? 32768 : 0);
    if (o) v = v - ((v >> 4) * 5243 >> 12) + OW[i];
    u[i] = v; w = mx(w, v);
  }
  for (i = 0; i < 8; ++i) {
    const y = (u[i] - w >> 6) * 94548 >> 10, f = y & 65535, j = f >> 12;
    z += p[i] = (EX[j] + ((EX[j + 1] - EX[j]) * (f & 4095) >> 12) << 8) >> mn(31, -(y >> 16));
  }
  for (i = 0; i < 8; ++i) {
    const q = idiv(z, p[i]), k4 = q < 300 ? fdiv(z, p[i]) >> 12 : 4800;
    p[i] = mx(11, mn(9990, (k4 * (o ? 126 : 128) >> 4) + (k4 * k4 * (o ? 48 : 49) >> 16) + 8 >> 4));
  }
  return p;
}
// Tenths as text: 225 '22.5', 290 '29' ('29.0' with d), 11 '1.1'.
const ot = (v, d) => { const s = '' + v; return s.slice(0, -1) + (d || v % 10 ? '.' + s.slice(-1) : ''); };
globalThis.derby = {OV: OV, field: field, race: race, step: step, order: order, odds: odds, idiv: idiv, fdiv: fdiv, D: D};
if (typeof pocket !== 'undefined')
  pocket.app.load('prog'), pocket.app.load('view'), pocket.app.load('scene'), pocket.app.load('pan'), pocket.app.load('play');
