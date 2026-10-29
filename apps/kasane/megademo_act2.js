// MEGADEMO chunk 2: Act II's scenes (TWIST, ZENITH, CORE, LOST), the plan
// lists (procMegademo, what the host tests read, is host-only:
// tools/kasane_contract/megademo_host.js).
'use strict';
// night() is 1 on the first night; each WAKE adds one.
let sortie = 742;
const night = () => sortie - 741;
// The cat's ship (a head with two ears, from behind) in 8 roll bins.
const SHIP = [0, -4, 5, -12, 7, -5, 13, 0, 10, 5, 5, 8, 2, 6, 0, 9, -2, 6, -5, 8, -10, 5,
  -13, 0, -7, -5, -5, -12, 0, -4];
function points(b) {
  const x = [], y = [], a = (b - 3.5) * .14, c = 2 * M.cos(a) * 16384, n = 2 * M.sin(a) * 16384;
  for (let i = 0; i < SHIP.length; i += 2) { x.push(SHIP[i]); y.push(SHIP[i + 1]); }
  return {kind: 'affineQ14Points', x: x, y: y, color: WH,
    coeff: [rnd(c), rnd(-n), rnd(n), rnd(c), 120 * 16384, 110 * 16384]};
}
// TWIST: frames, rails, arches, planks (two colours alternating), 2nd
// rails, 2 wall courses; x: the negative past the wall (inverted colours).
const RV = [0xc35f, 0x7a1a, 0xde5f, 0x5954];
function corrList(k, x) {
  const n = k.n, a = RV[1] ^ x, d = RV[3] ^ x;
  return [['fr', n - k.kf, RV[0] ^ x], ['ra', n, a], ['ar', n - k.ka, k.s, RV[2] ^ x], ['ru', n * k.m, d, d + a],
    ['ra', n, a], ['ra', n, d], ['ra', n, d]];
}
// The corridor: each point of the cross-section (2 x 2.5) is multiplied by
// a complex step per frame; the spacing tightens from Act I's gates (.43)
// to the river's over 48 frames. 120: the wall hits (white), then the
// negative (plans 8..14) until the stars burst out (196).
function twist(k, t) {
  if (t === 120 || t === 121) return BG(0xd71e);
  const ng = t > 121 && t < 196, e = (t - 200) / 40;
  BG((t < 60 || t > 120 ? t > 196 ? 0 : 0x0021 : t < 100 ? 0x0823 : 0x1045) ^ (ng ? 0xffff : 0));
  if (t < 232) {
    // L: all the plans (from 220 only frames and arches).
    const o = ng ? 8 : 0, L = t < 220, w = M.sin(PI * t / 240), d = k.D / k.n * w * w,
      r = k.r - (k.r - .43) * (1 - smooth(ramp(t, 0, 48))), ph = (t * .16666666666666666) % 1,
      g = M.pow(r, 1 - ph) * 265, A = .5 * M.sin(2 * PI * t / 240) - ph * d, c = M.cos(A) * g, s = M.sin(A) * g;
    // The cross-section's corners P, S, C, Q, turned and scaled.
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
  // Three rings of warp streaks rush out of the vanishing point.
  if (t >= 196) for (let j = 0; j < 3; ++j) {
    const f = ((t - 196) * .06 + j / 3) % 1, r = 6 + 150 * f * f;
    E(15, 120, 67, r, r + 3 + 40 * f * f, j * 1.1 + t * .01, 24, PI / 12, f < .4 ? RV[0] : WH);
  }
  river(t);
}
// ZENITH (LETHE on screen), t from -24: the dive from LETHE's mark down to
// the fleet (U, the pull-up run backwards); rolls (40..63), lasers, the
// lock (72); a salvo (96..111), the hit (112, kept small: the burst is
// CORE's), a roll out of it (128..147); the missile (150) and the pull-up
// after it (160) back to the mark. (The corner radar went for the heap
// the ship's model and tone needed, 2026-10-29.)
function zen(z, t) {
  const k = t - 112, e = ramp(t, 112, 150), ee = 1 - (1 - e) ** 3, T = 2 * PI * t / 96,
    U = t < 0 ? smooth(ramp(-t, 0, 16)) : smooth(ramp(t, 160, 190));
  BG(0);
  let w = .55 * M.sin(T);
  if (t >= 40 && t < 64) { const u = (t - 40) / 24; w += 2 * PI * u * u * (3 - 2 * u); }
  if (t >= 128 && t < 148) { const u = (t - 128) / 20; w -= 2 * PI * u * u * (3 - 2 * u); }
  const c = M.cos(w), s = M.sin(w), h = 8 * M.sin(2 * T) + 160 * U, h0 = 120 - h * s, h1 = 67 + h * c,
    x = 34 + 24 * M.sin(t * .09) - 120, y = 28 + 10 * M.cos(t * .05) - 67,
    px = 120 + x * c - y * s, py = 67 + x * s + y * c, sx = 46 * c - 6 * s, sy = 46 * s + 6 * c,
    b = M.max(0, M.min(7, rnd(-M.atan2(s, c) / .14 + 3.5))), cw = M.cos(t * .3), sw = M.sin(t * .3);
  E(0, 190 * c, 190 * s, -70 * s, 70 * c, t * .05, 120, 67);
  if (U < 1) {
    E(3, h0, h1, -s, c, 260 * c, 260 * s, 2.2 * M.pow(1.32, (t * .25) % 1), z.gl);
    E(4, h0, h1, -s, c, c, s, -(z.ln - 1) * .11 - ((t * .04) % 1) * .22, 150);
    // The city, and four more of it along the horizon to either side.
    for (let j = -2; j < 3; ++j)
      E(5, h0 + 58 * j * c, h1 + 58 * j * s, -s, c, c, s, 4.5 - .3 * j * j, M.min(1, t / 70) * 2.2);
  }
  // The formation; after the hit it scatters, spinning, and shrinks away.
  if (e < 1) {
    const g = 1 + 3 * ee, a = t * .3 + 9 * ee, r = 10 * (1 - e);
    E(1, px, py, sx * g, sy * g, r * M.cos(a), r * M.sin(a), .35, r);
  }
  if (t >= 0 && t < 96 && t % 16 < 5) E(6, 120, 100, px, py, sx, sy, 20, -40);
  else if (t >= 96 && k < 0) {
    const u = smooth(ramp(t, 96, 111));
    E(6, 120, 100, 120 + (px - 120) * u, 100 + (py - 100) * u, sx * u, sy * u, 20 + 40 * (1 - u), -40 - 30 * u);
  }
  E(7 + b, 120, 120, 120, 123 + (t % 3) * 2);
  // The blast at the formation's middle: one ring of short rays, streaks.
  if (k >= 0 && e < 1) {
    const R = 4 + 70 * ee, m = (z.sa - 1) / 2, qx = px + sx * m, qy = py + sy * m;
    E(2, qx, qy, R, R + 3, t * .05, 32, PI / 16, e < .3 ? OR : WH);
    E(2, qx, qy, 6 + 40 * ee, 14 + 70 * ee, k * .03, 12, PI / 6, OR);
  }
  // LETHE's mark: 160 above the horizon (overhead in the fight, ahead at
  // either end), an octahedron in the reticle; the tag follows (TGX).
  const a = 160 - h, X = 120 + a * s, Y = 67 - a * c;
  TGX = NaN;
  if (Y > -20 && Y < 150) {
    E(1, X, Y, 0, 0, 7 * cw, 7 * sw, .35, 7);
    E(2, X, Y, 11, 16, PI / 4, 4, PI / 2, OR);
    TGX = X + 10; TGY = Y - 6;
  }
  // The missile (150): out of the belly, weaving up after the mark, its
  // smoke a trail from the ship.
  if (t >= 150) {
    const u = smooth(ramp(t, 150, 190)) * .55, q = 8 * M.sin((t - 150) * .4) * (1 - u),
      cx = 120 + (X - 120) * u + q, cy = 112 + (Y - 112) * u;
    E(6, 120, 114, cx, cy, 0, 0, 6, -12);
    E(2, cx, cy, 0, 4 - 2 * u, M.atan2(Y - cy, X - cx), 2, PI, WH);
  }
}
// CORE: an icosahedral shell opens round a pulsing core (cube and
// octahedron); grey flashes, scan noise and a dolly zoom build to 100
// (docs/apps/megademo-story.md). pocket.shapes projects (pr), the plans
// draw lines. Icosahedron: vertices as sign/golden-ratio codes (i = -1,
// p/q = +/-phi), faces as hex vertex triples; Q: the cube's and
// octahedron's quads (cube corner i is the bits of i, octahedron corner i
// the axis i % 3 signed by i < 3); the ship: V its vertices (x right, y
// down, z ahead; the tail at -3), a character each, (code - 79) / 5 (nose,
// tail left/right/top/bottom, wing roots and tips, the two ears' base,
// base and tip, the flame), T its triangles as hex vertex triples (body
// x4, wings, ears, flame).
// FC, the fixed numbers, worked out once (no string decoded in a frame):
// per face its centre, |centre| and corners less the centre (13 a face,
// 0..259); then model points, copied into WM each frame (QV, SV): the
// quads' corners (28 points) and the ship's triangles' (27).
// WM / WP points: shell 0..59 (even faces first, then odd: each set has
// its own twist), quads 60..87, ship 88..114 and again 115..141 (projected
// with the depth less .8: NaN marks a corner too near the lens).
const FC = new Float32Array(425), QV = FC.subarray(260, 344), SV = FC.subarray(344);
{
  const p = (1 + M.sqrt(5)) / 2, n = M.sqrt(1 + p * p), C = {i: -1, '0': 0, '1': 1, p: p, q: -p};
  const S = 'ip01p0iq01q00ip01p0iq01qp0ip01q0iq01', F = '0b505101707a0ab1595b4ba2a7671839434232636838949524b62a867981', V = [];
  for (let i = 0; i < 36; ++i) V.push(C[S[i]] / n);
  for (let i = 0, o = 0; i < 20; ++i, o += 13) {
    const u = 3 * hx(F, 3 * i), v = 3 * hx(F, 3 * i + 1), w = 3 * hx(F, 3 * i + 2);
    for (let a = 0; a < 3; ++a) FC[o + a] = V[u + a] / 3 + V[v + a] / 3 + V[w + a] / 3;
    FC[o + 3] = M.hypot(FC[o], FC[o + 1], FC[o + 2]);
    for (let a = 0; a < 9; ++a) FC[o + 4 + a] = V[(a < 3 ? u : a < 6 ? v : w) + a % 3] - FC[o + a % 3];
  }
  const Q = '0132457604152637014302531245', W = 'OOrGO@WO@OI@OS@IOW1QBUOWmQBIM@MKIGAAUM@QKIWAALN?RN?OS=',
    T = '0130230140245167289abcdefgh';
  for (let i = 0, o = 260; i < 28; ++i, o += 3) {
    const c = hx(Q, i), n = c < 3 ? 1 : -1;
    FC[o] = i < 16 ? c & 1 ? 1 : -1 : c % 3 ? 0 : n;
    FC[o + 1] = i < 16 ? c & 2 ? 1 : -1 : c % 3 === 1 ? n : 0;
    FC[o + 2] = i < 16 ? c & 4 ? 1 : -1 : c % 3 === 2 ? n : 0;
  }
  for (let i = 0; i < 81; ++i) FC[344 + i] = (W.charCodeAt(3 * hx(T, i / 3 | 0) + i % 3) - 79) / 5;
}
// The projection: yaw (Ycy, Ysy), pitch (Pcp, Psp), depth Dd, roll (Rc,
// Rs). pr() puts it, with the yaw (cw, sa) and screen scale K at depth
// Dd, as pocket.shapes' matrix into WQ (x Rc - w Rs, x Rs + w Rc over
// Dd + q, round (120, 67)), and projects n points of WM from i into WP.
let Ycy, Ysy, Pcp, Psp, Dd, Rc = 1, Rs = 0;
function pr(cw, sa, K, i, n) {
  const x = -sa * Pcp, z = cw * Pcp;
  WQ[0] = 120 * x + K * (Rc * cw - Rs * sa * Psp); WQ[1] = 120 * Psp - K * Rs * Pcp;
  WQ[2] = 120 * z + K * (Rc * sa + Rs * cw * Psp); WQ[3] = 120 * Dd;
  WQ[4] = 67 * x + K * (Rs * cw + Rc * sa * Psp); WQ[5] = 67 * Psp + K * Rc * Pcp;
  WQ[6] = 67 * z + K * (Rs * sa - Rc * cw * Psp); WQ[7] = 67 * Dd;
  WQ[8] = x; WQ[9] = Psp; WQ[10] = z; WQ[11] = Dd;
  SH.project(WQ, WM, WP, i, n);
}
function core(k, t) {
  // Clock g: stop-motion from 86 (every 3 frames), stops at 100. The ship
  // rams the core at 112: the shell falls in, slow then sudden (m,
  // 112..118), and bursts in slow motion on its own clock (e over 118..168,
  // eased out: a pop, then a long drift) that freezes 124..129 (a still in
  // greys, the ring the one colour); then the light (168..).
  // cr: criticality, d: dolly zoom, o/op: opening, b: twist, A/B: yaw/
  // pitch, ng: negative frames, l: the roll dodging the core's beams
  // (64..84; one whole turn 74..84), gy: greys.
  const g = t < 86 ? t : t < 100 ? 86 + 3 * ((t - 86) / 3 | 0) : 100, step = t >= 86 && t < 100 && g === t;
  const ng = t === 100 || t === 101 || t === 118 || t === 119, m = ramp(t, 112, 118) ** 2,
    gy = t > 123 && t < 130 || t > 175;
  const cr = ramp(g, 60, 100), d = ramp(g, 84, 100) ** 2, e = ramp(t < 124 ? t : t < 130 ? 124 : t - 6, 118, 162),
    ee = 1 - (1 - e) ** 2;
  const sc = (18 + 44 * smooth(ramp(g, 0, 40))) * (1 - .5 * m), o = smooth(ramp(g, 58, 72)) + .6 * smooth(ramp(g, 76, 100));
  const op = o * (1 - m) + 2.2 * ee, b = o * (.5 + 1.5 * cr) * (1 - m) + 4 * ee;
  const A = g * .03 + 2 * cr ** 3, B = .5 + g * .011, X = ng ? 0xffff : 0,
    l = t >= 72 && t < 84 ? 2 * PI * smooth(ramp(t, 74, 84)) + .4 * M.sin((t - 72) * .5) * M.sin(PI * (t - 72) / 12) : 0;
  // Grey flashes as 9 cr^3 crosses an integer (60..86: the beams grazing),
  // dark grey on the steps, light grey negatives, the burst's afterglow.
  // A held breath (168..175: the debris all but still, a point of light
  // trembling in the middle), then the core's light past the panel's
  // white, told as an eye's (a sensor's) answer to it: blown out to CRT
  // white at once (0xd71e, 176..177, nothing drawn), the iris easing down
  // to its light (178..189; the lids squint, patch), the light swelling
  // slowly and then all at once (190..205, cubed), held until the set
  // shuts.
  const fl = t === 60 || t > 60 && t < 86 && (9 * cr ** 3 | 0) > (9 * ramp(t - 1, 60, 100) ** 3 | 0),
    u = t < 178 ? 1 : t < 190 ? 1 - smooth(ramp(t, 177, 186)) : ramp(t, 190, 206) ** 2, v = rnd(9 * (1 - e) ** 2);
  BG(ng ? 0xad55 : t > 175 ? rnd(26 * u) << 11 | rnd(56 * u) << 5 | rnd(30 * u) : fl ? 0x5acc : step ? 0x2945 :
    t > 119 ? v << 11 | v << 6 | v : 0);
  if (t > 175 && t < 178 || t > 205) return;
  // The dolly zoom snaps back on the ram.
  Ycy = M.cos(A); Ysy = M.sin(A); Pcp = M.cos(B); Psp = M.sin(B); Dd = 3.2 - 1.6 * d * (1 - m);
  Rc = M.cos(l); Rs = M.sin(l);
  // Each face shrunk (h) and pushed out along its normal (k), into WM
  // (face i at point 3 (i % 2 * 10 + i / 2)); twisted by b, one way or
  // the other, which is one turn with the yaw (the two sets' pr()). A
  // face's depth sum (ts, light or shade) is its centre's.
  const h = (1 - .35 * M.min(op, 1.6)) * (1 - .6 * ee), Cp = M.cos(A + b), Sp = M.sin(A + b), Cm = M.cos(A - b),
    Sm = M.sin(A - b);
  let c = 0, tm = 0;
  for (let i = 0, o = 0; i < 20; ++i, o += 13) {
    const k = 1 + op * (.8 + .3 * (i % 4)) / FC[o + 3], cx = FC[o] * k, cy = FC[o + 1] * k, H = FC[o + 2] * k;
    if (cy * Psp + (H * (i & 1 ? Cp : Cm) - cx * (i & 1 ? Sp : Sm)) * Pcp > 0) tm |= 1 << i;
    for (let j = 0, p = o + 4, w = 9 * (i % 2 * 10 + (i >> 1)); j < 3; ++j, p += 3, w += 3) {
      WM[w] = cx + FC[p] * h; WM[w + 1] = cy + FC[p + 1] * h; WM[w + 2] = H + FC[p + 2] * h;
    }
  }
  pr(Cm, Sm, sc * Dd, 0, 30); pr(Cp, Sp, sc * Dd, 30, 30);
  for (let i = 0; i < 20; ++i) {
    const w = 6 * (i % 2 * 10 + (i >> 1)), x0 = WP[w], y0 = WP[w + 1], x1 = WP[w + 2], y1 = WP[w + 3], x2 = WP[w + 4],
      y2 = WP[w + 5];
    // Shards off the screen (or behind the lens: NaN) are not drawn, nor,
    // in the burst, past 4000 steps: lines cost their length unclipped.
    if (!(x0 > -80 && x0 < 320 && y0 > -73 && y0 < 207 && x1 > -80 && x1 < 320 && y1 > -73 && y1 < 207 &&
      x2 > -80 && x2 < 320 && y2 > -73 && y2 < 207) || e > 0 && (c += M.max(M.abs(x0 - x1), M.abs(y0 - y1)) +
      M.max(M.abs(x1 - x2), M.abs(y1 - y2)) + M.max(M.abs(x2 - x0), M.abs(y2 - y0))) > 4000) continue;
    // In the burst's glare only light and shade are left: shards grey,
    // light facing the eye, dark turned away (the ring is the one colour).
    const ts = tm >> i & 1;
    E(0, x0, y0, x1, y1, x2, y2, gy ? ts ? 0x4208 : 0xad55 : (ts ? 0x167f : i % 3 ? 0xfa3f : WH) ^ X);
  }
  // The core (cc): cube and octahedron (LETHE's magenta), pulsing; a quad
  // with a corner behind the lens (NaN) is not drawn.
  const cc = sc * smooth(ramp(g, 58, 72)) * (1 + .15 * M.sin(g * .5)) * (1 + .5 * cr) * (1 - m);
  if (cc > 1) {
    WM.set(QV, 180);
    pr(Ycy, Ysy, cc * .33 * Dd, 60, 16); pr(Ycy, Ysy, cc * .55 * Dd, 76, 12);
    for (let q = 0, w = 120; q < 7; ++q, w += 8) {
      const x0 = WP[w], y0 = WP[w + 1], x1 = WP[w + 2], y1 = WP[w + 3], x2 = WP[w + 4], y2 = WP[w + 5], x3 = WP[w + 6],
        y3 = WP[w + 7];
      if (x0 + y0 + x1 + y1 + x2 + y2 + x3 + y3 === x0 + y0 + x1 + y1 + x2 + y2 + x3 + y3)
        E(q > 3 ? 2 : q < 2 ? 1 : 3, x0, y0, x1, y1, x2, y2, x3, y3);
    }
  }
  // In the burst, a shockwave ring and rays thrown out; before it, rays
  // from 64 and three beams sweeping round (64..84) to dodge.
  if (e > 0) {
    const R = 6 + 150 * ee, rr = M.max(8, k.r), ln = rnd(k.r * (1 - e));
    E(4, 120, 67, R, R + 3 + 9 * (1 - e), e * 3, rr, 2 * PI / rr, gy || e < .3 ? OR : WH ^ X);
    if (ln) E(4, 120, 67, 10 + 260 * ee, 40 + 300 * ee, g * .07, ln, 2 * PI / ln, gy ? 0x8410 : OR ^ X);
    // The core's light: dense rays from the middle, a blot of white that
    // swells over the screen.
    if (t > 167) {
      E(4, 120, 67, 0, t < 176 ? 2 + .4 * (t - 168) + M.sin(t * 2.1) : t < 190 ? 6 + 1.2 * (t - 178) :
        22 + 128 * ramp(t, 190, 206) ** 3, t * .3, rr, 2 * PI / rr, WH);
    }
    if (t > 177) {
      // The ship's after-image near the eye, against the light, in its
      // yellow's complement (blue), sinking into the white as it swells (a), its faces
      // drifting apart and dropping out (q).
      const a = 1 - ramp(t, 194, 206) ** 2, q = 1.6 * ramp(t, 178, 206);
      Ycy = 0; Ysy = .6; Dd = 12; Rc = Pcp = 1; Rs = Psp = 0;
      const sm = sp();
      for (let i = 0, w = 176; i < 8; ++i, w += 6)
        if (q < 1 + i % 3 * .3 && sm >> i & 1) {
          const x = 30 * q * M.sin(i * 2.3), y = q * (12 * M.cos(i * 1.7) + 4);
          E(0, WP[w] + x, WP[w + 1] + y, WP[w + 2] + x, WP[w + 3] + y, WP[w + 4] + x, WP[w + 5] + y,
            rnd(26 * u * (1 - a)) << 11 | rnd(56 * u * (1 - a)) << 5 | rnd(30 * u * (1 - a) + 18 * a));
        }
    }
  } else {
    const rr = rnd(k.r * smooth(ramp(g, 64, 96)) * (1 - .6 * smooth(ramp(t, 78, 88))));
    for (let j = 0; j < k.d && rr; ++j)
      E(4, 120, 67, cc * .4, 30 + 120 * cr, g * .07 + j * PI / rr + l, rr, 2 * PI / rr, (fl ? OR : WH) ^ X);
    if (t >= 72 && t < 84) E(4, 120, 67, cc * .5, 260, t * .09 + l, 3, 2 * PI / 3, OR);
  }
  // Dust falls in on the real clock (the stop-motion is the shell's, not
  // dropped frames): 8 rings, r = sqrt of the fall, swirling.
  const p = rnd(k.r / 4 * smooth(ramp(t, 76, 86)) * (1 - m)), T = t < 100 ? t : 100;
  for (let i = 0; i < 8 && p; ++i) {
    const u = (i / 8 - T / 36) % 1 + 1, f = M.sqrt(u % 1), r = 24 + 116 * f;
    E(4, 120, 67, r, r + 4 + 12 * (1 - f), i * 1.7 + 30 / (r + 9), p, 2 * PI / p, (i & 1 ? 0x07ff : 0xfd20) ^ X);
  }
  // Stars stretch in the zoom and rush past in the burst (G); noise (L).
  const G = 1 + .8 * d * (1 - m) + 1.4 * ee, L = rnd(k.n * (t < 100 ? cr ** 1.5 : t < 118 ? 0 : 1 - e));
  E(5, 190 * G * Rc, 190 * G * Rs, -70 * G * Rs, 70 * G * Rc, g * .05 + 2 * ee, 120, 67);
  if (L) E(6, t * 1.7, L, 10 + 40 * cr);
  if (e > 0) return;
  // LETHE's reticle: on the shell, onto the core as the lasers open it
  // (58..72); the tag follows (TGX) until the charge.
  const R = sc * 1.05 + (cc * .75 - sc * 1.05) * smooth(ramp(t, 58, 72));
  TGX = NaN;
  if (t < 84) { E(4, 120, 67, R, R + 6, PI / 4, 4, PI / 2, OR); TGX = 120 + R * .7; TGY = 67 - R * .7; }
  // The missile, chased from behind (0..46). It never reaches the shell:
  // a ripple runs out of the shell (18..40), the missile shudders, and
  // something unseen bends it away to the right, turning over, out of the
  // frame (28..44); it flickers violet in the grip.
  {
    const f = ramp(t, 18, 40), r = sc * 1.1 + 90 * f;
    if (f > 0 && f < 1) E(4, 120, 67, r, r + 2 + 5 * f, t * .05, 24, PI / 12, 0xc81f);
  }
  if (t < 46) {
    // Weaving, shuddering (w, 22..28), thrown off to the right (v); the
    // smoke a line back along its heading.
    const v = ramp(t, 28, 44), w = ramp(t, 22, 28) * (1 - v), r = 2.4 * v * v + .3 * w * M.sin(t * 2.3),
      x = 120 + 7 * M.sin(t * .25) * (1 - ramp(t, 0, 44)) + 170 * v * v + 2.5 * w * M.sin(t * 2.9),
      y = 104 - 46 * v + 2 * w * M.cos(t * 3.7), c = M.cos(r), s = M.sin(r), f = 8 + (t % 3) * 2;
    E(0, x, y + 4, x - 30 * s, y + 30 * c, x - 30 * s, y + 30 * c, 0x8c71);
    E(0, x + 10 * s, y - 10 * c, x - 4 * c - 3 * s, y - 4 * s + 3 * c, x + 4 * c - 3 * s, y + 4 * s + 3 * c,
      t > 21 && t < 36 && t & 1 ? 0xc81f : WH);
    E(0, x - 2 * c - 4 * s, y - 2 * s + 4 * c, x + 2 * c - 4 * s, y + 2 * s + 4 * c, x - f * s, y + f * c, OR);
  }
  if (t < 44) return;
  // The ship from behind, a 3D model in a wide lens (focal 110): it sweeps
  // in from beside the camera, its tail looming (44..56), fires four
  // volleys (58..72) that open the shell, banks through the beams, and
  // charges (84..100) 300 units deep, so perspective alone shrinks it to
  // a point while the dolly zoom throws the rest outward. It hangs there
  // in the time stop; at 112 it hits.
  const q = smooth(ramp(t, 44, 56)), w = ramp(T, 84, 100), a = .35 * (1 - q);
  Ycy = 3.4 * (1 - q); Ysy = 1.6 + 1.4 * (1 - q) - 1.6 * w; Dd = 4.6 + 5.4 * q + 300 * w * w;
  const r = a + (T >= 72 && T < 84 ? .4 * M.sin((T - 72) * .5) : 0);
  Rc = M.cos(r); Rs = M.sin(r); Pcp = M.cos(a); Psp = -M.sin(a);
  const sm = sp();
  // Four volleys (58..72): a beam from each wing root and tip (vertices
  // 5..8: the wings' first and last corners, WP 200, 204, 206, 210), a
  // quarter of the way at a time, to the shell's middle.
  for (let v = 58, f; v < 72; v += 4)
    if ((f = (t - v) / 4) >= 0 && f < 1)
      for (let j = 5; j < 9; ++j) {
        const n = j < 7 ? 200 + 4 * (j - 5) : 206 + 4 * (j - 7), X = WP[n], Y = WP[n + 1], b = .7 * f, e = b + .3,
          x = 120 + 4 * (j - 6.5) - X, y = 67 - Y;
        E(0, X + x * b, Y + y * b, X + x * e, Y + y * e, X + x * e, Y + y * e, OR);
      }
  // Screentone first (a manga's dot tone): the underside, wings and ears,
  // the faces that lie under or against another, as 8 rows of dots (plan
  // 7; 36 a face, the dots are segments), so the tone coarsens as the
  // ship looms. Then every outline over it.
  for (let i = 2, w = 188; i < 8; ++i, w += 6)
    if (sm >> i & 1) E(7, WP[w], WP[w + 1], (WP[w + 2] - WP[w]) / 8, (WP[w + 3] - WP[w + 1]) / 8, (WP[w + 4] - WP[w + 2]) / 8,
      (WP[w + 5] - WP[w + 3]) / 8, 8, 1);
  for (let i = 0, w = 176; i < 9; ++i, w += 6)
    if (sm >> i & 1) E(0, WP[w], WP[w + 1], WP[w + 2], WP[w + 3], WP[w + 4], WP[w + 5], i > 7 ? T % 3 ? OR : 0xfd20 : 0xffe0);
}
// sp(): the ship's 9 triangles (WM 88..114, a copy at 115..141) rolled
// (Rc, Rs), turned (Pcp, Psp), placed at (Ycy, Ysy, Dd) and projected
// (focal 110) into WP; returns a bit per triangle that can be drawn: no
// corner within .8 of the lens (the copy, projected with the depth less
// .8, is NaN there) or past the plans' -480..720.
function sp() {
  WM.set(SV, 264); WM.set(SV, 345);
  const a = -Psp * Rc, b = Psp * Rs;
  WQ[0] = 120 * a + 110 * Pcp * Rc; WQ[1] = 120 * b - 110 * Pcp * Rs; WQ[2] = 120 * Pcp + 110 * Psp;
  WQ[3] = 120 * Dd + 110 * Ycy; WQ[4] = 67 * a + 110 * Rs; WQ[5] = 67 * b + 110 * Rc; WQ[6] = 67 * Pcp;
  WQ[7] = 67 * Dd + 110 * Ysy; WQ[8] = a; WQ[9] = b; WQ[10] = Pcp; WQ[11] = Dd - .8;
  SH.project(WQ, WM, WP, 115, 27);
  WQ[11] = Dd;
  SH.project(WQ, WM, WP, 88, 27);
  let m = 0;
  for (let i = 0, w = 176; i < 9; ++i, w += 6) {
    const x0 = WP[w], y0 = WP[w + 1], x1 = WP[w + 2], y1 = WP[w + 3], x2 = WP[w + 4], y2 = WP[w + 5];
    if (WP[w + 54] === WP[w + 54] && WP[w + 56] === WP[w + 56] && WP[w + 58] === WP[w + 58] && x0 > -400 && x0 < 640 &&
      y0 > -400 && y0 < 640 && x1 > -400 && x1 < 640 && y1 > -400 && y1 < 640 && x2 > -400 && x2 < 640 && y2 > -400 &&
      y2 < 640) m |= 1 << i;
  }
  return m;
}
// LOST: CRT off, a line to a dot to black; WAKE's closed eyes follow.
function lost(t) {
  BG(0);
  const w = t < 10 ? 2 + 118 * (1 - t / 10) ** 2 : 2.5 - (t - 10) / 4, v = w * .8;
  if (w > 0) { E(0, 120 - w, 67, 120 + w, 67); E(1, 120 - v, 66, 120 + v, 66); E(1, 120 - v, 68, 120 + v, 68); }
}
// Act I (CALL, ROUTE, VOYAGE) keeps the log name NEWS; WAKE plays first.
const NAMES = ['NEWS', 'NEWS', 'NEWS', 'TWIST', 'ZENITH', 'CORE', 'LOST', 'WAKE'],
  LEN = [64, 56, 96, 240, 216, 226, 24, 104];
// Scene s's plans as [template, args...]. Act I's set is its HUD, then
// TWIST's first 8: they register at the end of WAKE (the heap is lowest
// there) and pass to TWIST at the cut (enter()). The loader builds plan i
// when it registers it, so no list waits in the heap.
function plist(s, tier) {
  const k = KN[tier], z = k.z, w = s < 4 ? corrList(k.c, 0).concat([['st', z.st]]) : null;
  return s < 3 ? [['cv', 0x57f9], ['ch', 0x57f9], ['eg'], ['cw', WH], ['sp', 0xfde0], ['sc', 0x57f9]].concat(w) :
    s === 3 ? w.concat(corrList(k.c, 0xffff), [['ry']]) :
    s === 5 ? [['tri'], ['qd', 0x17a7], ['qd', 0xf81f], ['sg', 0x17a7], ['ry'], ['st', z.st], ['nz', k.k.n], ['ht', 0xa4e0]] :
    s === 6 ? [['ln', 0xef9f], ['ln', 0x167f]] : s === 7 ? [] :
    // ZENITH: what its opening shot draws (stars, LETHE's octahedron and
    // reticle) first, as the plans register a frame at a time.
    [['st', z.st], ['sa', z.sa], ['ry'], ['gr'], ['la', z.ln], ['ci', z.cr, z.cc, 3.2 / z.cc], ['tr', z.sa, z.ts],
      ['sh', 0], ['sh', 1], ['sh', 2], ['sh', 3], ['sh', 4], ['sh', 5], ['sh', 6], ['sh', 7]];
}
// Scene s at tier and frame t, through E and BG.
function play(s, tier, t) {
  const k = KN[tier];
  if (s < 3) brief(s, t); else if (s === 3) twist(k.c, t); else if (s === 4) zen(k.z, t - 24);
  else if (s === 5) core(k.k, t); else if (s === 6) lost(t); else wake(t);
}
