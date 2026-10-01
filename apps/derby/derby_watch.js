// DERBY WATCH: a night race on a 1000 m straight in pseudo-3D line art.
// Why and numbers: apps/derby/README.md, docs/apps/derby-watch.md
// This entry is the race model; the game (pocket.kasane) is in the
// chunks.txt scripts, loaded while this evaluates: one parse at a time.
'use strict';
const M = Math, PI = M.PI, sin = M.sin, flo = M.floor, rnd = M.round, mx = M.max, mn = M.min;
const D = 1000, DT = .05, U = 1 / 6;
// Race model, tuned by tools/games/tune_derby.mjs (README has the terms).
const TOP = 16.6, SPR = .2, ST0 = .8, ST1 = 1, KI0 = .02, KI1 = .015, PACE = .015, FORM = .6, WR = .995,
  WS = .02, FADE = .97, EB = .97, PK0 = .03, PK1 = .5, WIND = 1.5, KS = .25, BG = 4, BK = .15;
// Lane depths grow geometrically: one MUL walks them in the VM (gate).
const Q = 1.085, DL = [], DNR = 11, DFR = 22.6;
for (let j = 0; j < 8; ++j) DL.push(12 * M.pow(Q, j));
const POOL = 'BIT DASH,HEX HOOF,NULL MOON,BAUD STAR,LOOP KID,CACHE HIT,TIN KITE,OPCODE,PIXEL RUN,STACK TOP,BYTE MARE,MOSS DRUM'.split(',');
const SILK = [0xffff, 0x8c71, 0xf800, 0x237f, 0xffe0, 0x07e0, 0xfd20, 0xf81f], STY = ['FRONT', 'STALK', 'CLOSE'];
const z8 = v => [v, v, v, v, v, v, v, v];
function rng(s) {
  return function () {
    s = s + 0x6d2b79f5 | 0;
    let t = M.imul(s ^ s >>> 15, 1 | s);
    t = t + M.imul(t ^ t >>> 7, 61 | t) ^ t;
    return ((t ^ t >>> 14) >>> 0) / 4294967296;
  };
}
// The course (README "楕円"): oval with chance OV[0]. On its bend (273..650 m
// of a runner's own x) its progress is scaled by its lane (OV[1]) and a draw
// (OV[2]) from a stream of its own (c), which also picks the course.
const OV = [.5, .0025, .02];
function field(seed) {
  const r = rng(seed), c = rng(seed + 0x2545f491), p = POOL.slice(), h = [], m = [];
  for (let i = 0; i < 8; ++i) {
    h.push({n: p.splice(flo(r() * p.length), 1)[0], top: TOP + SPR * r(), st: ST0 + ST1 * r(), kick: KI0 + KI1 * r(),
      sty: flo(r() * 3), re: .25 * r(), acc: 5 + 2 * r(), coat: [0xdd8c, 0xc460, 0xbdf7, 0xef5b][flo(r() * 4)]});
    m[i] = (1 + OV[1] * (3.5 - i) / 3.5) * (1 + (2 * c() - 1) * OV[2]);
  }
  return {seed: seed, h: h, m: m, o: c() < OV[0]};
}
// Arithmetic only, no Math.sin/exp: a seed replays bit for bit anywhere.
// Per race a form offset the odds cannot see, and a slow random walk.
function race(f) {
  const r = rng(f.seed + 0x5bd1e995), fm = z8(0), e = [];
  for (let i = 0; i < 8; ++i) { e[i] = f.h[i].st; fm[i] = (r() - .5) * FORM; }
  return {t: 0, x: z8(0), v: z8(0), e: e, sb: z8(0), px: z8(0), tc: z8(0), fm: fm, w: z8(0),
    r: r, done: 0};
}
function step(f, s, dt) {
  const t0 = s.t, xl = mx.apply(null, s.x);
  s.t += dt;
  for (let i = 0; i < 8; ++i) {
    const h = f.h[i], x = s.x[i], top = h.top + s.fm[i], gap = xl - x, early = x < D * (.5 + .08 * h.sty);
    let v = s.v[i];
    s.px[i] = x;
    if (s.t < h.re) continue;
    // Early the field bunches and the leader pays for the wind; late, a
    // runner within BG m of the lead digs in.
    let g = s.e[i] <= 0 ? top * FADE : early ? top * (1 - PACE * h.sty) + mn(PK1, gap * PK0) : top * (1 + h.kick * (1 + KS * h.sty));
    if (!early && gap < BG) g += gap * BK;
    g += s.w[i] = s.w[i] * WR + (s.r() - .5) * WS;
    if (s.r() < .0006) s.sb[i] = .5;
    if (s.sb[i] > 0) s.sb[i] -= dt, g *= .9;
    const a = g - v, up = h.acc * dt;
    v += a > up ? up : a < -dt ? -dt : a;
    if (v > EB * h.top) s.e[i] -= (v / h.top - EB) * (gap < 1 ? WIND : 1) * dt;
    const y = s.x[i] = x + (s.v[i] = v) * dt * (f.o && x >= 273 && x < 650 ? f.m[i] : 1);
    if (x < D && y >= D) s.tc[i] = t0 + (D - x) / (y - x) * dt, ++s.done;
  }
}
const order = s => [0, 1, 2, 3, 4, 5, 6, 7].sort((a, b) => (s.tc[a] || 1e9) - (s.tc[b] || 1e9) || s.x[b] - s.x[a] || a - b);
// Win chance: a Luce model of the paddock figures (weights fitted), 20% take;
// on the oval (o) a temperature, the lane and a take refitted (README).
function odds(h, o) {
  const p = [];
  let z = 0, i, k;
  for (i = 0; i < 8; ++i) k = h[i], z += p[i] = M.exp((o ? .92 : 1) * (17.2 * k.top + 2.6 * k.st - 2.3 * k.re + .5 * (k.acc + (k.sty > 1))) + (o ? .15 * (3.5 - i) / 3.5 : 0));
  for (i = 0; i < 8; ++i) k = z / p[i], p[i] = mx(1.1, mn(999, rnd((o ? 7.87 : 8) * k * (1 + .0015 * k)) / 10));
  return p;
}
globalThis.derby = {OV: OV, field: field, race: race, step: step, order: order, odds: odds, D: D, DT: DT};
if (typeof pocket !== 'undefined')
  pocket.app.load('prog'), pocket.app.load('view'), pocket.app.load('scene'), pocket.app.load('pan'), pocket.app.load('play');
