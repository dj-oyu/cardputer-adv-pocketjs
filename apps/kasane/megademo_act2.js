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
    // L: all the plans (the corridor thins to frames and arches from 220).
    const o = ng ? 8 : 0, L = t < 220, w = M.sin(PI * t / 240), d = k.D / k.n * w * w,
      r = k.r - (k.r - .43) * (1 - smooth(ramp(t, 0, 48))), ph = (t * .16666666666666666) % 1,
      g = M.pow(r, 1 - ph) * 265, A = .5 * M.sin(2 * PI * t / 240) - ph * d, c = M.cos(A) * g, s = M.sin(A) * g;
    // The cross-section's corners P, S, C, Q, turned and scaled. (Names
    // are reused across the app's functions: each distinct one is a
    // resident atom.)
    const px = 1.2 * c - .95 * s, py = 1.2 * s + .95 * c, sx = -.8 * c - .95 * s, sy = -.8 * s + .95 * c,
      cx = -.8 * c + 1.55 * s, cy = -.8 * s - 1.55 * c, qx = 1.2 * c + 1.55 * s, qy = 1.2 * s - 1.55 * c;
    // h = r e^(i d), the step per frame; l e^(i a) its m-th root (a rung's
    // step); x + iy = .8^kf e^(i d kf) (the first frame drawn), u + iv =
    // .8^ka e^(i d ka) (the first arch).
    const h0 = r * M.cos(d), h1 = r * M.sin(d), l = M.pow(r, 1 / k.m), a = d * (1 / k.m),
      n = M.pow(.8, k.kf), b = d * k.kf, x = n * M.cos(b), y = n * M.sin(b),
      m = M.pow(.8, k.ka), q = d * k.ka, u = m * M.cos(q), v = m * M.sin(q);
    E(o, px * x - py * y, px * y + py * x, sx * x - sy * y, sx * y + sy * x, cx * x - cy * y, cx * y + cy * x, h0, h1);
    if (L) {
      E(o + 1, px, py, sx, sy, h0, h1);
      E(o + 3, px, py, sx, sy, l * M.cos(a), l * M.sin(a));
    }
    E(o + 4, cx, cy, qx, qy, h0, h1);
    if (L) E(o + 5, sx + (cx - sx) * .3333333333333333, sy + (cy - sy) * .3333333333333333,
      px + (qx - px) * .3333333333333333, py + (qy - py) * .3333333333333333, h0, h1);
    E(o + 6, sx + (cx - sx) * .6666666666666666, sy + (cy - sy) * .6666666666666666,
      px + (qx - px) * .6666666666666666, py + (qy - py) * .6666666666666666, h0, h1);
    const X = (cx - sx) / 2, Y = (cy - sy) / 2;
    E(o + 2, cx * u - cy * v, cx * v + cy * u, qx * u - qy * v, qx * v + qy * u, X * u - Y * v, X * v + Y * u, h0, h1);
  }
  if (e > 0) { const g = e * e; E(7, 190 * g, 0, 0, 70 * g, (t - 240) * .05, 120, 67); }
  // The wire opens: three rings of warp streaks rush out of the vanishing
  // point (faster and longer as they go), from 196 to the cut.
  if (t >= 196) for (let j = 0; j < 3; ++j) {
    const f = ((t - 196) * .06 + j / 3) % 1, r = 6 + 150 * f * f;
    E(15, 120, 67, r, r + 3 + 40 * f * f, j * 1.1 + t * .01, 24, PI / 12, f < .4 ? RV[0] : WH);
  }
  river(t);
}
// ZENITH (LETHE on screen): the ship rolls (a barrel roll in 40..63) and
// pitches over the city, lasers at the formation; 72 locks on. Then a
// salvo (96..111: four trails grow from the ship to the octahedra), the
// hit at 112 (a grey flash, a shockwave ring and streaks thrown out, the
// formation scattered spinning), a roll the other way out of the blast
// (128..147), and from 160 the ship pulls up: the city falls away until
// only the stars are left (CORE opens on them). The radar is drawn in the
// image's corner (a second surface cost two 10 KB frames).
const RC0 = 28, RC1 = 108;
function zen(z, t) {
  const k = t - 112, e = ramp(t, 112, 150), ee = 1 - (1 - e) ** 3, U = smooth(ramp(t, 160, 190)), T = 2 * PI * t / 96;
  // The hit: two grey frames, then a warm glow fading out.
  const glow = k > 1 ? rnd(10 * (1 - ramp(k, 2, 16)) ** 2) : 0;
  BG(k === 0 || k === 1 ? 0x5acc : glow << 11 | glow << 5);
  let w = .55 * M.sin(T);
  if (t >= 40 && t < 64) { const u = (t - 40) / 24; w += 2 * PI * u * u * (3 - 2 * u); }
  if (t >= 128 && t < 148) { const u = (t - 128) / 20; w -= 2 * PI * u * u * (3 - 2 * u); }
  const c = M.cos(w), s = M.sin(w), h = 8 * M.sin(2 * T) + 160 * U, h0 = 120 - h * s, h1 = 67 + h * c,
    x = 34 + 24 * M.sin(t * .09) - 120, y = 28 + 10 * M.cos(t * .05) - 67,
    px = 120 + x * c - y * s, py = 67 + x * s + y * c, sx = 46 * c - 6 * s, sy = 46 * s + 6 * c,
    b = M.max(0, M.min(7, rnd(-M.atan2(s, c) / .14 + 3.5))), cw = M.cos(t * .3), sw = M.sin(t * .3);
  E(0, 190 * c, 190 * s, -70 * s, 70 * c, t * .05, 120, 67);
  if (U < 1) {
    E(1, h0, h1, -s, c, 260 * c, 260 * s, 2.2 * M.pow(1.32, (t * .25) % 1), z.gl);
    E(2, h0, h1, -s, c, c, s, -(z.ln - 1) * .11 - ((t * .04) % 1) * .22, 150);
    E(3, h0, h1, -s, c, c, s, 4.5, M.min(1, t / 70) * 2.2);
  }
  // The formation; after the hit it scatters, spinning, and shrinks away.
  if (e < 1) {
    const g = 1 + 3 * ee, a = t * .3 + 9 * ee, r = 10 * (1 - e);
    E(4, px, py, sx * g, sy * g, r * M.cos(a), r * M.sin(a), .35, r);
  }
  if (t < 96 && t % 16 < 5) E(5, 120, 100, px, py, sx, sy, 20, -40);
  else if (t >= 96 && k < 0) {
    const u = smooth(ramp(t, 96, 111));
    E(5, 120, 100, 120 + (px - 120) * u, 100 + (py - 100) * u, sx * u, sy * u, 20 + 40 * (1 - u), -40 - 30 * u);
  }
  E(6 + b, 120, 120, 120, 123 + (t % 3) * 2);
  // The blast: a white starburst (8 frames), a double ring of short rays
  // (orange, then white) and streaks thrown out.
  if (k >= 0 && e < 1) {
    const R = 4 + 140 * ee, m = (z.sa - 1) / 2, qx = px + sx * m, qy = py + sy * m;
    if (k < 8) E(16, qx, qy, 2, 12 + 50 * smooth(k / 8) * (1 - k / 8), k * .2, 24, PI / 12, WH);
    E(16, qx, qy, R, R + 4, t * .05, 64, PI / 32, e < .3 ? OR : WH);
    E(16, qx, qy, R * .8, R * .8 + 3, -t * .05, 48, PI / 24, OR);
    E(16, qx, qy, 6 + 90 * ee, 20 + 150 * ee, k * .03, 16, PI / 8, e < .5 ? WH : OR);
  }
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
  const u = x * Ycy + z * Ysy, v = z * Ycy - x * Ysy, w = y * Pcp - v * Psp, q = y * Psp + v * Pcp,
    f = s * Dd / M.max(Dd + q, Dd * .4);
  PJ[j] = 120 + u * f; PJ[j + 1] = 67 + w * f;
  return q;
}
function core(k, t) {
  // Clock g: steps every 3 frames from 86 (stop-motion), stops at 100.
  // At 112 the shell falls in (im), at 115 it bursts (e). Negative frames:
  // the stop and the burst.
  const g = t < 86 ? t : t < 100 ? 86 + 3 * ((t - 86) / 3 | 0) : 100, step = t >= 86 && t < 100 && g === t;
  const ng = t === 100 || t === 101 || t === 115 || t === 116, m = smooth(ramp(t, 112, 115));
  const cr = ramp(g, 60, 100), d = ramp(g, 84, 100) ** 2, e = ramp(t, 115, 133), ee = 1 - (1 - e) ** 3;
  const sc = (18 + 44 * smooth(ramp(g, 0, 40))) * (1 - .5 * m), o = smooth(ramp(g, 40, 56)) + .6 * smooth(ramp(g, 76, 100));
  const op = o * (1 - m) + 3.6 * ee, b = o * (.5 + 1.5 * cr) * (1 - m) + 5 * ee;
  const A = g * .03 + 2 * cr ** 3, B = .5 + g * .011, X = ng ? 0xffff : 0;
  // Grey flashes: one frame each time 9 u^3 crosses an integer, u over
  // 60..86. Backdrop: those flashes, dark grey on the steps, light grey
  // negatives, and the burst's glow fading back to black.
  const fl = t === 60 || t > 60 && t < 86 && (9 * cr ** 3 | 0) > (9 * ramp(t - 1, 60, 100) ** 3 | 0),
    glow = rnd(9 * (1 - ramp(t, 117, 129)) ** 2);
  BG(ng ? 0xad55 : fl ? 0x5acc : step ? 0x2945 : t >= 117 && glow ? glow << 11 | glow << 6 | glow : 0);
  // The dolly zoom snaps back in the fall.
  Ycy = M.cos(A); Ysy = M.sin(A); Pcp = M.cos(B); Psp = M.sin(B); Dd = 3.2 - 1.6 * d * (1 - m);
  const h = (1 - .35 * M.min(op, 1.6)) * (1 - .6 * ee);
  let c = 0;
  for (let i = 0; i < 20; ++i) {
    // Shrink each face (h), push it out along its normal (l), twist it
    // like a petal (a).
    const u = 3 * hx(IFS, 3 * i), v = 3 * hx(IFS, 3 * i + 1), w = 3 * hx(IFS, 3 * i + 2),
      cx = IV[u] / 3 + IV[v] / 3 + IV[w] / 3, cy = IV[u + 1] / 3 + IV[v + 1] / 3 + IV[w + 1] / 3,
      H = IV[u + 2] / 3 + IV[v + 2] / 3 + IV[w + 2] / 3,
      l = op * (.8 + .3 * (i % 4)) / M.hypot(cx, cy, H), a = b * (i & 1 ? 1 : -1), cw = M.cos(a), sa = M.sin(a);
    let ts = 0, D = 0;
    for (let j = 0; j < 3; ++j) {
      const q = j ? j > 1 ? w : v : u, x = cx + (IV[q] - cx) * h + cx * l, y = cy + (IV[q + 1] - cy) * h + cy * l,
        z = H + (IV[q + 2] - H) * h + H * l;
      ts += pj(x * cw + z * sa, y, z * cw - x * sa, sc, 2 * j);
      if (M.abs(PJ[2 * j] - 120) > 200 || M.abs(PJ[2 * j + 1] - 67) > 140) D = 1e4;
    }
    // Lines are not clipped and cost their length (8192 a frame): shards
    // off the screen, or past 4000 steps in the burst, are not drawn.
    D += M.max(M.abs(PJ[0] - PJ[2]), M.abs(PJ[1] - PJ[3]));
    D += M.max(M.abs(PJ[2] - PJ[4]), M.abs(PJ[3] - PJ[5]));
    D += M.max(M.abs(PJ[4] - PJ[0]), M.abs(PJ[5] - PJ[1]));
    if (D >= 1e4 || e > 0 && (c += D) > 4000) continue;
    E(0, PJ[0], PJ[1], PJ[2], PJ[3], PJ[4], PJ[5], (ts > 0 ? BL : i % 3 ? VI : WH) ^ X);
  }
  // The core: cube (green) and octahedron (orange), counter-rotating,
  // pulsing; the falling shell crushes it. Cube vertex i is the bits of i,
  // octahedron vertex i the axis i % 3 signed by i < 3.
  const cc = sc * smooth(ramp(g, 40, 56)) * (1 + .15 * M.sin(g * .5)) * (1 + .5 * cr) * (1 - m);
  if (cc > 1) for (let q = 0; q < 7; ++q) {
    const O = q > 3, s = cc * (O ? .55 : .33);
    for (let j = 0; j < 4; ++j) {
      const i = hx(QD, 4 * q + j), n = i < 3 ? 1 : -1;
      if (O) pj(i % 3 ? 0 : n, i % 3 === 1 ? n : 0, i % 3 === 2 ? n : 0, s, 2 * j);
      else pj(i & 1 ? 1 : -1, i & 2 ? 1 : -1, i & 4 ? 1 : -1, s, 2 * j);
    }
    E(O ? 2 : q < 2 ? 1 : 3, PJ[0], PJ[1], PJ[2], PJ[3], PJ[4], PJ[5], PJ[6], PJ[7]);
  }
  // Rays from frame 64; in the burst, a shockwave ring and the rays thrown
  // out to the edges.
  if (e > 0) {
    const R = 6 + 150 * ee, rr = M.max(8, k.r), ln = rnd(k.r * (1 - e));
    E(4, 120, 67, R, R + 3 + 9 * (1 - e), t * .02, rr, 2 * PI / rr, (e < .3 ? OR : WH) ^ X);
    if (ln) E(4, 120, 67, 10 + 260 * ee, 40 + 300 * ee, g * .07, ln, 2 * PI / ln, OR ^ X);
  } else {
    const rr = rnd(k.r * smooth(ramp(g, 64, 96)) * (1 - m) * (1 - .6 * smooth(ramp(t, 78, 88))));
    for (let j = 0; j < k.d && rr; ++j)
      E(4, 120, 67, cc * .4, 30 + 120 * cr, g * .07 + j * PI / rr, rr, 2 * PI / rr, (fl ? OR : WH) ^ X);
  }
  // Dust falls into the core on the real clock (stop-motion is the shell's,
  // not dropped frames): 8 rings of streaks, r = sqrt of the fall so it
  // speeds up near the core, swirling as it goes in; held in the stop,
  // crushed in the fall.
  const p = rnd(k.r / 4 * smooth(ramp(t, 76, 86)) * (1 - m)), T = t < 100 ? t : 100;
  for (let i = 0; i < 8 && p && t < 115; ++i) {
    const u = (i / 8 - T / 36) % 1 + 1, f = M.sqrt(u % 1), r = (24 + 116 * f) * (1 - m);
    E(4, 120, 67, r, r + 4 + 12 * (1 - f), i * 1.7 + 30 / (r + 9), p, 2 * PI / p, (i & 1 ? 0x07ff : 0xfd20) ^ X);
  }
  // Stars stretch outwards in the dolly zoom and rush past in the burst;
  // the noise is still in the stop and spends itself in the burst.
  const G = 1 + .8 * d * (1 - m) + 1.4 * ee, L = rnd(k.n * (t < 100 ? cr ** 1.5 : t < 115 ? 0 : 1 - e));
  if (t < 132) E(5, 190 * G, 0, 0, 70 * G, g * .05 + 2 * ee, 120, 67);
  if (L) E(6, t * 1.7, L, 10 + 40 * cr);
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
  LEN = [64, 56, 96, 240, 192, 140, 24, 104];
// Scene s's plans as [template, args...] (sh: the ship's roll bin, its
// points). Act I's scenes share one set: its HUD (the world is
// pocket.shapes'), then TWIST's first 8 (the river and the stars), which
// register with it at the end of WAKE, where the heap is lowest, and pass
// to TWIST at the cut (enter()). The loader builds plan i when it
// registers it (plan()), so no list is kept while it waits.
function plist(s, tier) {
  const k = KN[tier], z = k.z, w = s < 4 ? corrList(k.c, 0).concat([['st', z.st]]) : null;
  return s < 3 ? [['cv', 0x57f9], ['ch', 0x57f9], ['eg'], ['cw', WH], ['sp', 0xfde0], ['sc', 0x57f9]].concat(w) :
    s === 3 ? w.concat(corrList(k.c, 0xffff), [['ry']]) :
    s === 5 ? [['tri'], ['qd', GR], ['qd', OR], ['sg', GR], ['ry'], ['st', z.st], ['nz', k.k.n]] :
    s === 6 ? [['ln', WH], ['ln', BL]] : s === 7 ? [] :
    [['st', z.st], ['gr'], ['la', z.ln], ['ci', z.cr, z.cc, 3.2 / z.cc], ['sa', z.sa], ['tr', z.sa, z.ts],
      ['sh', 0], ['sh', 1], ['sh', 2], ['sh', 3], ['sh', 4], ['sh', 5], ['sh', 6], ['sh', 7],
      ['rd', z.rr], ['sw', z.sa, RC0, RC1], ['ry']];
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
