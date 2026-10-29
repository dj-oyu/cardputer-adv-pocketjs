// MEGADEMO chunk 2: Act II's scenes (TWIST, ZENITH, CORE, LOST) drawn
// through E/BG (megademo_act1.js), the plan lists, and procMegademo (what
// the host tests read).
'use strict';
// The nights: night() is 1 on the first (each WAKE adds one; the loop's
// first WAKE is the first night). The wall's tally, the sortie number and
// the clouding memory (megademo_view.js) read it.
let sortie = 742;
const night = () => sortie - 741;
// The cat's ship (a head with two ears, seen from behind) in 8 roll bins:
// a rotated sprite table, built when the plan registers.
const SHIP = [0, -4, 5, -12, 7, -5, 13, 0, 10, 5, 5, 8, 2, 6, 0, 9, -2, 6, -5, 8, -10, 5,
  -13, 0, -7, -5, -5, -12, 0, -4];
function points(b) {
  const x = [], y = [], a = (b - 3.5) * .14, c = 2 * M.cos(a) * 16384, n = 2 * M.sin(a) * 16384;
  for (let i = 0; i < SHIP.length; i += 2) { x.push(SHIP[i]); y.push(SHIP[i + 1]); }
  return {kind: 'affineQ14Points', x: x, y: y, color: WH,
    coeff: [rnd(c), rnd(-n), rnd(n), rnd(c), 120 * 16384, 110 * 16384]};
}
// TWIST: frames, rails, arches, planks, 2nd rails, 2 wall courses, in the
// river's violets; planks alternate two (the plan swaps c for (c + c2) - c).
// x: the negative (past the wall), the same plans in inverted colours.
const RV = [0xc35f, 0x7a1a, 0xde5f, 0x5954];
function corrList(k, x) {
  const n = k.n, a = RV[1] ^ x, d = RV[3] ^ x;
  return [['fr', n - k.kf, RV[0] ^ x], ['ra', n, a], ['ar', n - k.ka, k.s, RV[2] ^ x], ['ru', n * k.m, d, d + a],
    ['ra', n, a], ['ra', n, d], ['ra', n, d]];
}
// The corridor: every point of a cross-section (Q0: 2 x 2.5) is multiplied
// by the complex z per frame; the first frame and arch drawn (kf, ka) keep
// their place as r changes. The first 48 frames tighten the spacing from
// Act I's gates (.43) to the river's. At its deepest (120) two white
// frames: the wall (river(), brief.js) hits, and past it the river is a
// negative (plans 8..14) until the stars burst from the vanishing point
// (196); the corridor thins to its frames and arches (220), then nothing.
function twist(k, t) {
  if (t === 120 || t === 121) return BG(WH);
  const ng = t > 121 && t < 196, e = (t - 200) / 40;
  BG((t < 60 || t > 120 ? t > 196 ? 0 : 0x0021 : t < 100 ? 0x0823 : 0x1045) ^ (ng ? 0xffff : 0));
  if (t < 232) {
    const o = ng ? 8 : 0, all = t < 220, w = M.sin(PI * t / 240), dl = k.D / k.n * w * w,
      r = k.r - (k.r - .43) * (1 - smooth(ramp(t, 0, 48))), ph = (t * .16666666666666666) % 1,
      g = M.pow(r, 1 - ph) * 265, an = .5 * M.sin(2 * PI * t / 240) - ph * dl, c = M.cos(an) * g, s = M.sin(an) * g;
    // The cross-section's corners, turned and scaled.
    const ax = 1.2 * c - .95 * s, ay = 1.2 * s + .95 * c, bx = -.8 * c - .95 * s, by = -.8 * s + .95 * c,
      cx = -.8 * c + 1.55 * s, cy = -.8 * s - 1.55 * c, dx = 1.2 * c + 1.55 * s, dy = 1.2 * s - 1.55 * c;
    // z = r e^(i dl); zf its m-th root (a rung's step); zk, za: .8^kf, .8^ka.
    const zr = r * M.cos(dl), zi = r * M.sin(dl), fm = M.pow(r, 1 / k.m), fa = dl * (1 / k.m),
      km = M.pow(.8, k.kf), ka = dl * k.kf, kr = km * M.cos(ka), ki = km * M.sin(ka),
      am = M.pow(.8, k.ka), aa = dl * k.ka, ar = am * M.cos(aa), ai = am * M.sin(aa);
    E(o, ax * kr - ay * ki, ax * ki + ay * kr, bx * kr - by * ki, bx * ki + by * kr, cx * kr - cy * ki, cx * ki + cy * kr, zr, zi);
    if (all) {
      E(o + 1, ax, ay, bx, by, zr, zi);
      E(o + 3, ax, ay, bx, by, fm * M.cos(fa), fm * M.sin(fa));
    }
    E(o + 4, cx, cy, dx, dy, zr, zi);
    if (all) E(o + 5, bx + (cx - bx) * .3333333333333333, by + (cy - by) * .3333333333333333,
      ax + (dx - ax) * .3333333333333333, ay + (dy - ay) * .3333333333333333, zr, zi);
    E(o + 6, bx + (cx - bx) * .6666666666666666, by + (cy - by) * .6666666666666666,
      ax + (dx - ax) * .6666666666666666, ay + (dy - ay) * .6666666666666666, zr, zi);
    const hx = (cx - bx) / 2, hy = (cy - by) / 2;
    E(o + 2, cx * ar - cy * ai, cx * ai + cy * ar, dx * ar - dy * ai, dx * ai + dy * ar, hx * ar - hy * ai, hx * ai + hy * ar, zr, zi);
  }
  if (e > 0) { const g = e * e; E(7, 190 * g, 0, 0, 70 * g, (t - 240) * .05, 120, 67); }
  river(t);
}
// ZENITH (LETHE on screen): the ship rolls (a barrel roll in 40..63) and
// pitches; the radar is drawn in the image's corner (a second surface cost
// two 10 KB frames).
const RC0 = 28, RC1 = 108;
function zen(z, t) {
  BG(0);
  const T = 2 * PI * t / 96;
  let roll = .55 * M.sin(T);
  if (t >= 40 && t < 64) { const u = (t - 40) / 24; roll += 2 * PI * u * u * (3 - 2 * u); }
  const c = M.cos(roll), s = M.sin(roll), hz = 8 * M.sin(2 * T), h0 = 120 - hz * s, h1 = 67 + hz * c,
    x = 34 + 24 * M.sin(t * .09) - 120, y = 28 + 10 * M.cos(t * .05) - 67,
    px = 120 + x * c - y * s, py = 67 + x * s + y * c, sx = 46 * c - 6 * s, sy = 46 * s + 6 * c,
    b = M.max(0, M.min(7, rnd(-M.atan2(s, c) / .14 + 3.5))), cw = M.cos(t * .3), sw = M.sin(t * .3);
  E(0, 190 * c, 190 * s, -70 * s, 70 * c, t * .05, 120, 67);
  E(1, h0, h1, -s, c, 260 * c, 260 * s, 2.2 * M.pow(1.32, (t * .25) % 1), z.gl);
  E(2, h0, h1, -s, c, c, s, -(z.ln - 1) * .11 - ((t * .04) % 1) * .22, 150);
  E(3, h0, h1, -s, c, c, s, 4.5, M.min(1, t / 70) * 2.2);
  E(4, px, py, sx, sy, 10 * cw, 10 * sw, .35, 10);
  if (t % 16 < 5) E(5, 120, 100, px, py, sx, sy, 20, -40);
  E(6 + b, 120, 120, 120, 123 + (t % 3) * 2);
  E(14, RC0, RC1, 22 / 1.45 ** (z.rr - 1));
  E(15, RC0 + 22 * cw, RC1 + 22 * sw, RC0 + (px - 120) * .15, RC1 + (py - 67) * .15, sx * .15, sy * .15);
}
// CORE: an icosahedral shell opens round a pulsing core (cube and
// octahedron); the climax adds grey flashes, scan noise and a short dolly
// zoom, eased in cubically towards frame 100 (docs/apps/megademo-story.md).
// The 3D is projected here; the plans only draw lines. Icosahedron:
// vertices as sign/golden-ratio codes (i = -1, p/q = +/-phi), faces as hex
// vertex triples; the cube and octahedron from bits (QD: their quads).
const IV = [], IFS = '0b505101707a0ab1595b4ba2a7671839434232636838949524b62a867981',
  QD = '0132457604152637014302531245';
{
  const p = (1 + M.sqrt(5)) / 2, n = M.sqrt(1 + p * p), C = {i: -1, '0': 0, '1': 1, p: p, q: -p};
  const S = 'ip01p0iq01q00ip01p0iq01qp0ip01q0iq01';
  for (let i = 0; i < 36; ++i) IV.push(C[S[i]] / n);
}
// Projection state for this frame: yaw, pitch, camera distance D. pj()
// rotates (yaw, then pitch), projects at scale s into PJ[j], PJ[j + 1] and
// returns z (> 0 is away from the camera); a petal past the lens must not
// blow up.
let Ycy, Ysy, Pcp, Psp, Dd;
const PJ = [0, 0, 0, 0, 0, 0, 0, 0];
function pj(x, y, z, s, j) {
  const x1 = x * Ycy + z * Ysy, z0 = z * Ycy - x * Ysy, y1 = y * Pcp - z0 * Psp, z1 = y * Psp + z0 * Pcp,
    f = s * Dd / M.max(Dd + z1, Dd * .4);
  PJ[j] = 120 + x1 * f; PJ[j + 1] = 67 + y1 * f;
  return z1;
}
function core(k, t) {
  // Clock g: steps every 3 frames from 86 (stop-motion), stops at 100.
  // At 112 the shell falls in (im), at 115 it bursts (e). Negative frames:
  // the stop and the burst.
  const g = t < 86 ? t : t < 100 ? 86 + 3 * ((t - 86) / 3 | 0) : 100, step = t >= 86 && t < 100 && g === t;
  const neg = t === 100 || t === 101 || t === 115 || t === 116, im = smooth(ramp(t, 112, 115));
  const crit = ramp(g, 60, 100), dz = ramp(g, 84, 100) ** 2, e = ramp(t, 115, 133), ee = 1 - (1 - e) ** 3;
  const sc = (18 + 44 * smooth(ramp(g, 0, 40))) * (1 - .5 * im), o0 = smooth(ramp(g, 40, 56)) + .6 * smooth(ramp(g, 76, 100));
  const op = o0 * (1 - im) + 3.6 * ee, tw = o0 * (.5 + 1.5 * crit) * (1 - im) + 5 * ee;
  const ya = g * .03 + 2 * crit ** 3, pa = .5 + g * .011, iv = neg ? 0xffff : 0;
  // Grey flashes: one frame each time 9 u^3 crosses an integer, u over
  // 60..86. Backdrop: those flashes, dark grey on the steps, light grey
  // negatives, and the burst's glow fading back to black.
  const fl = t === 60 || t > 60 && t < 86 && (9 * crit ** 3 | 0) > (9 * ramp(t - 1, 60, 100) ** 3 | 0),
    glow = rnd(9 * (1 - ramp(t, 117, 129)) ** 2);
  BG(neg ? 0xad55 : fl ? 0x5acc : step ? 0x2945 : t >= 117 && glow ? glow << 11 | glow << 6 | glow : 0);
  // The dolly zoom snaps back in the fall.
  Ycy = M.cos(ya); Ysy = M.sin(ya); Pcp = M.cos(pa); Psp = M.sin(pa); Dd = 3.2 - 1.6 * dz * (1 - im);
  const h = (1 - .35 * M.min(op, 1.6)) * (1 - .6 * ee);
  let cost = 0;
  for (let i = 0; i < 20; ++i) {
    // Shrink each face (h), push it out along its normal (l), twist it
    // like a petal (a).
    const u = 3 * hx(IFS, 3 * i), v = 3 * hx(IFS, 3 * i + 1), w = 3 * hx(IFS, 3 * i + 2),
      cx = IV[u] / 3 + IV[v] / 3 + IV[w] / 3, cy = IV[u + 1] / 3 + IV[v + 1] / 3 + IV[w + 1] / 3,
      cz = IV[u + 2] / 3 + IV[v + 2] / 3 + IV[w + 2] / 3,
      l = op * (.8 + .3 * (i % 4)) / M.hypot(cx, cy, cz), a = tw * (i & 1 ? 1 : -1), ca = M.cos(a), sa = M.sin(a);
    let zs = 0, far = 0;
    for (let j = 0; j < 3; ++j) {
      const q = j ? j > 1 ? w : v : u, x = cx + (IV[q] - cx) * h + cx * l, y = cy + (IV[q + 1] - cy) * h + cy * l,
        z = cz + (IV[q + 2] - cz) * h + cz * l;
      zs += pj(x * ca + z * sa, y, z * ca - x * sa, sc, 2 * j);
      if (M.abs(PJ[2 * j] - 120) > 200 || M.abs(PJ[2 * j + 1] - 67) > 140) far = 1e4;
    }
    // Lines are not clipped and cost their length (8192 a frame): shards
    // off the screen, or past 4000 steps in the burst, are not drawn.
    far += M.max(M.abs(PJ[0] - PJ[2]), M.abs(PJ[1] - PJ[3]));
    far += M.max(M.abs(PJ[2] - PJ[4]), M.abs(PJ[3] - PJ[5]));
    far += M.max(M.abs(PJ[4] - PJ[0]), M.abs(PJ[5] - PJ[1]));
    if (far >= 1e4 || e > 0 && (cost += far) > 4000) continue;
    E(0, PJ[0], PJ[1], PJ[2], PJ[3], PJ[4], PJ[5], (zs > 0 ? BL : i % 3 ? VI : WH) ^ iv);
  }
  // The core: cube (green) and octahedron (orange), counter-rotating,
  // pulsing; the falling shell crushes it. Cube vertex i is the bits of i,
  // octahedron vertex i the axis i % 3 signed by i < 3.
  const cs = sc * smooth(ramp(g, 40, 56)) * (1 + .15 * M.sin(g * .5)) * (1 + .5 * crit) * (1 - im);
  if (cs > 1) for (let q = 0; q < 7; ++q) {
    const oct = q > 3, s = cs * (oct ? .55 : .33);
    for (let j = 0; j < 4; ++j) {
      const i = hx(QD, 4 * q + j), n = i < 3 ? 1 : -1;
      if (oct) pj(i % 3 ? 0 : n, i % 3 === 1 ? n : 0, i % 3 === 2 ? n : 0, s, 2 * j);
      else pj(i & 1 ? 1 : -1, i & 2 ? 1 : -1, i & 4 ? 1 : -1, s, 2 * j);
    }
    E(oct ? 2 : q < 2 ? 1 : 3, PJ[0], PJ[1], PJ[2], PJ[3], PJ[4], PJ[5], PJ[6], PJ[7]);
  }
  // Rays from frame 64; in the burst, a shockwave ring and the rays thrown
  // out to the edges.
  if (e > 0) {
    const R = 6 + 150 * ee, nr = M.max(8, k.r), nb = rnd(k.r * (1 - e));
    E(4, 120, 67, R, R + 3 + 9 * (1 - e), t * .02, nr, 2 * PI / nr, (e < .3 ? OR : WH) ^ iv);
    if (nb) E(4, 120, 67, 10 + 260 * ee, 40 + 300 * ee, g * .07, nb, 2 * PI / nb, OR ^ iv);
  } else {
    const nr = rnd(k.r * smooth(ramp(g, 64, 96)) * (1 - im) * (1 - .6 * smooth(ramp(t, 78, 88))));
    for (let j = 0; j < k.d && nr; ++j)
      E(4, 120, 67, cs * .4, 30 + 120 * crit, g * .07 + j * PI / nr, nr, 2 * PI / nr, (fl ? OR : WH) ^ iv);
  }
  // Dust falls into the core on the real clock (stop-motion is the shell's,
  // not dropped frames): 8 rings of streaks, r = sqrt of the fall so it
  // speeds up near the core, swirling as it goes in; held in the stop,
  // crushed in the fall.
  const np = rnd(k.r / 4 * smooth(ramp(t, 76, 86)) * (1 - im)), tp = t < 100 ? t : 100;
  for (let i = 0; i < 8 && np && t < 115; ++i) {
    const u = (i / 8 - tp / 36) % 1 + 1, f = M.sqrt(u % 1), r = (24 + 116 * f) * (1 - im);
    E(4, 120, 67, r, r + 4 + 12 * (1 - f), i * 1.7 + 30 / (r + 9), np, 2 * PI / np, (i & 1 ? 0x07ff : 0xfd20) ^ iv);
  }
  // Stars stretch outwards in the dolly zoom and rush past in the burst;
  // the noise is still in the stop and spends itself in the burst.
  const gs = 1 + .8 * dz * (1 - im) + 1.4 * ee, nn = rnd(k.n * (t < 100 ? crit ** 1.5 : t < 115 ? 0 : 1 - e));
  if (t < 132) E(5, 190 * gs, 0, 0, 70 * gs, g * .05 + 2 * ee, 120, 67);
  if (nn) E(6, t * 1.7, nn, 10 + 40 * crit);
}
// LOST: CRT off. The line shrinks to a dot, then the dot goes; a few dark
// frames, and WAKE's closed eyelids go on from the same black.
function lost(t) {
  BG(0);
  const w = t < 10 ? 2 + 118 * (1 - t / 10) ** 2 : 2.5 - (t - 10) / 4, v = w * .8;
  if (w > 0) { E(0, 120 - w, 67, 120 + w, 67); E(1, 120 - v, 66, 120 + v, 66); E(1, 120 - v, 68, 120 + v, 68); }
}
// Act I (CALL, ROUTE, VOYAGE: megademo_brief.js) keeps the scene name NEWS
// for the logs' readers; WAKE (megademo_wake.js) plays first. LEN: frames.
const NAMES = ['NEWS', 'NEWS', 'NEWS', 'TWIST', 'ZENITH', 'CORE', 'LOST', 'WAKE'],
  LEN = [64, 56, 96, 240, 96, 140, 24, 104];
// Scene s's plans as [template, args...] (sh: the ship's roll bin, its
// points). Act I's scenes share one set (its HUD; the world is
// pocket.shapes'). cap: how many the prefetch takes (TWIST's negative
// registers during TWIST, not in VOYAGE's dive). The loader builds plan i
// when it registers it (plan()), so no list is kept while it waits.
function plist(s, tier) {
  const k = KN[tier], z = k.z;
  const L = s < 3 ? [['cv', 0x57f9], ['ch', 0x57f9], ['eg'], ['cw', WH], ['sp', 0xfde0], ['sc', 0x57f9]] :
    s === 3 ? corrList(k.c, 0).concat([['st', z.st]], corrList(k.c, 0xffff)) :
    s === 5 ? [['tri'], ['qd', GR], ['qd', OR], ['sg', GR], ['ry'], ['st', z.st], ['nz', k.k.n]] :
    s === 6 ? [['ln', WH], ['ln', BL]] : s === 7 ? [] :
    [['st', z.st], ['gr'], ['la', z.ln], ['ci', z.cr, z.cc, 3.2 / z.cc], ['sa', z.sa], ['tr', z.sa, z.ts],
      ['sh', 0], ['sh', 1], ['sh', 2], ['sh', 3], ['sh', 4], ['sh', 5], ['sh', 6], ['sh', 7],
      ['rd', z.rr], ['sw', z.sa, RC0, RC1]];
  if (s === 3) L.cap = 8;
  return L;
}
// [program, points or null] of a plan entry.
const plan = e => [prog(T[e[0]], e.slice(1)), e[0] === 'sh' ? points(e[1]) : null];
// Scene s at tier and frame t, through E and BG.
function play(s, tier, t) {
  const k = KN[tier];
  if (s < 3) brief(s, t); else if (s === 3) twist(k.c, t); else if (s === 4) zen(k.z, t);
  else if (s === 5) core(k.k, t); else if (s === 6) lost(t); else wake(t);
}
// The host tests' form: {b: backdrop, s: surface, d: [[plan, [inputs]]...]}
// (plan -1: a pocket.shapes segment, tools/kasane_contract/shape_draw_shim.h).
// specs(): a thunk per plan, the form the tests read.
globalThis.procMegademo = {names: NAMES, len: LEN, tiers: KN.length, specs: (s, tier) => plist(s, tier).map(e => () => plan(e)),
  frame: function (s, tier, t) {
  const r = {b: 0, s: 0, d: []}, e = E, g = BG;
  E = (k, ...a) => { r.d.push([k, a]); };
  BG = b => { r.b = b; };
  play(s, tier, t);
  E = e; BG = g;
  return r;
}};
