// MEGADEMO chunk: Act I (the briefing) and TWIST's overlays, drawn with
// pocket.shapes into WAKE's arrays. The TV is a cockpit display: the world
// (a floor grid, the route's gates, LETHE far off) under a HUD (tapes, a
// pitch ladder, five brain waves, a spectrum, a scope), all on one beat.
// CALL boots it from a lone carrier; ROUTE lights the gates near to far;
// VOYAGE cranes up over the whole route, the ship sets out and the camera
// drops in behind it. Words: numbers no one reads, and LETHE.
// docs/apps/megademo-story.md (the code is dense: its bytecode is heap).
'use strict';
// The backdrop: navy, going to the river's near-black in the dive.
const briefB = (s, t) => s > 1 && t > 84 ? 0x0021 : 0x0827, OCT = [0, 1, 2, 3, 0, 4, 1, 5, 2, 4, 3, 5, 0], BS = [108, 61, 113, 61, 116, 65, 120, 62, 124, 65, 127, 61, 132, 61];
const P2 = (i, x, y) => { WP[2 * i] = x; WP[2 * i + 1] = y; }, nz = i => (M.sin(i * 12.9898) * 43758.5453 % 1 + 1) % 1 - .5;
// The route on the floor (x across, z ahead, y up); LETHE at z 1660.
const rx = s => 110 * (1 - M.cos(s * .0042)) - 50 * (1 - M.cos(s * .009)), rdx = s => .462 * M.sin(s * .0042) - .45 * M.sin(s * .009);
// BW: WM holds the world (WAKE and river() write over it). WM points:
// 12 gates 0 (4 corners, the first again, a break), grid 72 (lines along
// the route cut in 5 points: one behind the camera drops only its edges),
// LETHE 114, ship 127, trail 132.
let BW = 0;
function world() {
  BW = 1;
  for (let i = 0; i < 12; ++i) {
    const s = 60 + 128 * i, x = rx(s), d = rdx(s), l = 60 / M.hypot(1, d);
    for (let j = 0; j < 6; ++j) {
      const g = (j & 3) % 3 ? l : -l;
      wpt(6 * i + j, j > 4 ? NaN : x + 12 + g, j & 2 && j < 4 ? 150 : 0, s - g * d);
    }
  }
  for (let j = 0; j < 24; ++j) wpt(72 + j, j % 6 > 4 ? NaN : -120 + 120 * (j / 6 | 0), 0, -60 + 400 * (j % 6));
  for (let m = 0; m < 18; ++m) wpt(96 + m, m % 3 > 1 ? NaN : m % 3 ? 240 : -120, 0, 100 + 280 * (m / 3 | 0));
}
// The camera [t, target x y z, distance, yaw, pitch]: down the route;
// VOYAGE goes up and round over all of it, holds while the ship sets out,
// then drops in behind the ship looking ahead (TWIST is next).
// The last key flies along the route (fly()). Gates are TWIST's cross-
// section (megademo_act2.js, Q0: 2 x 2.5 at 60 units, the camera 12 left
// of the middle and 57 up), so the cut finds the river's first frames.
const K = [[0, 0, 57, 120, 200, 0, 0], [44, 52, 0, 830, 1250, -1.5708, 1.05], [78, 70, 0, 880, 1150, -1.5708, 1], 0];
// It closes on the sixth gate (s 700) along its normal at 12.6 a frame,
// 123 short at the cut: TWIST's first frame, at its speed.
function fly(t) {
  const d = rdx(700), l = M.hypot(1, d), a = -23 - 12.6 * (96 - t);
  return [96, rx(700) + a * d / l, 57, 700 + a / l, 100, M.atan(d), 0];
}
// Scene s (0 CALL, 1 ROUTE, 2 VOYAGE) at t; emit as in wake() (a plan
// draw too: emit(plan, inputs), else [plan, inputs] in the list). The result
// carries LETHE's place on screen (its tag) and three readouts.
function brief(s, t, emit) {
  WD = []; WEMIT = emit || null;
  if (!BW) world();
  // g: Act I's clock (the beat runs on across its scenes). on(t0): an
  // instrument boots at t0 on CALL, flickering for 6 frames.
  // VOYAGE powers them down again in the dive (off at 82 + a stagger).
  const g = [0, 64, 120][s] + t, p = M.sin((g & 15) * .3927),
    on = t0 => s > 1 ? t < 82 + t0 % 9 || t < 88 + t0 % 9 && t & 1 : s || t >= t0 + 6 || t >= t0 && t & 1,
    sw = s > 1 ? 1 - smooth(ramp(t, 0, 20)) : 1, sh = 760 * ramp(t, 40, 96) ** 2;
  K[3] = fly(t);
  let i = 0;
  while (s > 1 && i < 2 && K[i + 1][0] <= t) ++i;
  const u = s > 1 ? smooth(ramp(t, K[i][0], K[i + 1][0])) : 0,
    c = K[i].map((v, j) => j === 4 ? v * M.pow(K[i + 1][j] / v, u) : v + (K[i + 1][j] - v) * u),
    yw = c[5] + .03 * p * sw, cy = M.cos(yw), sy = M.sin(yw), cp = M.cos(c[6]), sp = M.sin(c[6]),
    F = [sy * cp, -sp, cy * cp], R = [cy, 0, -sy], U = [sy * sp, cp, cy * sp],
    C = [c[1] - F[0] * c[4], c[2] + 2 * p * sw - F[1] * c[4], c[3] - F[2] * c[4]];
  for (let j = 0; j < 3; ++j) { WQ[j] = 120 * F[j] + 150 * R[j]; WQ[4 + j] = 67 * F[j] - 150 * U[j]; WQ[8 + j] = F[j]; }
  for (let r = 0; r < 12; r += 4) WQ[r + 3] = -(WQ[r] * C[0] + WQ[r + 1] * C[1] + WQ[r + 2] * C[2]);
  // LETHE (an octahedron turning, one stroke over its edges), the ship
  // (an arrowhead) and its dashes.
  const X = rx(1660), sx = rx(sh), sd = rdx(sh), sl = M.hypot(1, sd), A = [0, 26, 16, -12, 0, -4, -16, -12, 0, 26];
  for (let j = 0; j < 13; ++j) {
    const v = OCT[j], a = g * .04 + v * 1.5708;
    wpt(114 + j, v < 4 ? X + 46 * M.cos(a) : X, v < 4 ? 70 : v > 4 ? 10 : 130, v < 4 ? 1660 + 46 * M.sin(a) : 1660);
  }
  for (let j = 0; j < 14; ++j) {
    const a = sh - 34 * (j / 3 | 0) - 14 * (j % 3), b = j < 5 ? A[2 * j + 1] : 0, q = j < 5 ? A[2 * j] : 0;
    wpt(127 + j, j > 4 && (j % 3 > 1 || a < -20) ? NaN : j < 5 ? sx + (sd * b + q) / sl : rx(a + 20), 45, j < 5 ? sh + (b - sd * q) / sl : a + 20);
  }
  SH.project(WQ, WM, WP, 0, 141);
  // The world: the grid boots on CALL; ROUTE lights the gates near to far
  // (the newest flashes); VOYAGE shows them all as the camera rises.
  if (s > 1 || on(50)) SH.outline(WP, 72, 41, 0x29f4);
  const n = s > 1 ? 7 + rnd(5 * smooth(ramp(t, 8, 40))) : 7, lit = s > 1 ? n : s ? M.max(0, M.min(7, (t - 2) / 6 | 0)) : 0;
  // Into the river: the gates turn its violet as the camera comes down.
  const v = s > 1 ? smooth(ramp(t, 70, 92)) : 0, gc = rnd(24 * v) << 11 | rnd(63 - 37 * v) << 5 | 31;
  if (lit) SH.outline(WP, 0, 6 * lit - 1, gc);
  if (s && n > lit) SH.outline(WP, 6 * lit, 6 * (n - lit) - 1, 0x394f);
  if (s === 1 && lit && t < 46 && (t - 2) % 6 < 2) SH.outline(WP, 6 * lit - 6, 5, WH);
  let tag = null;
  if (s > 1 || s && (t >= 48 || t >= 42 && t & 1)) {
    SH.outline(WP, 114, 13, 0xf81f);
    const w = WQ[8] * X + WQ[9] * 70 + WQ[10] * 1660 + WQ[11], x = (WQ[0] * X + WQ[1] * 70 + WQ[2] * 1660 + WQ[3]) / w,
      y = (WQ[4] * X + WQ[5] * 70 + WQ[6] * 1660 + WQ[7]) / w;
    if (w > 0 && x > -30 && x < 236 && y > 10 && y < 134) tag = [x, y];
  }
  if (s > 1 && (t >= 40 || t >= 30 && t & 2)) { SH.outline(WP, 127, 5, WH); SH.outline(WP, 132, 9, 0x57f9); }
  // The HUD, plans over the world (megademo_act1.js, T): the carrier
  // (alone mid-screen on CALL, then up into the top strip, modulated),
  // heading, speed and height tapes, the pitch ladder (a rung each 10
  // degrees, the horizon long), five brain waves (their own rhythms, one
  // spike together on the beat; the middle one flattens as the ship
  // leaves), a spectrum and a scope.
  const D = (k, a) => WEMIT ? WEMIT(k, a) : WD.push([k, a]), cu = s ? 1 : smooth(ramp(t, 20, 36)),
    spd = g * .2 + (s > 1 ? 30 * ramp(t, 40, 96) ** 3 : 0), lv = (c[6] * 103 - 35) / 18;
  // Ticks from a0 to a1 across the line at: the short ones, then every
  // m-th long.
  const comb = (k, a0, a1, at, v, len, m) => {
    for (let r = 0, sp = 6 * (m > 5 ? 3 : 1); r < 2; ++r, v /= m, sp *= m) {
      const f = v - M.floor(v), p0 = a0 + (f > 0 ? 1 - f : 0) * sp;
      D(k, [at, r ? len : len * .45, p0, sp, M.max(0, M.floor((a1 - p0) / sp) + 1)]);
    }
  };
  D(3, [67 - 58 * cu, 14 - 11 * cu, -g * .45, s ? .4 : .4 * ramp(t, 12, 20), g * .21]);
  if (on(26)) comb(1, 70, 170, 17, c[5] * 11.5, -4, 5);
  if (on(28)) comb(0, 26, 96, 3, spd, 6, 5);
  if (on(30)) comb(0, 26, 96, 237, C[1] * .03, -6, 5);
  if (on(34)) {
    comb(0, 26, 96, 108, lv, -34, 18);
    comb(0, 26, 96, 132, lv, 34, 18);
    for (let j = 0; j < 7; ++j) P2(j, BS[2 * j], BS[2 * j + 1]);
    SH.outline(WP, 0, 7, 0x57f9);
  }
  for (let q = 0; q < 5; ++q) if (on(36 + 2 * q))
    D(2, [109 + 5 * q, q === 2 && s > 1 ? 1.3 - 1.3 * smooth(ramp(t, 48, 80)) : 1.3, 2 * g, [.5, .8, .33, 1.1, .65][q],
      [.37, .21, .53, .11, .29][q], [0x57f9, 0x07ff, 0xce59, 0xb81f, 0x5d7f][q]]);
  if (on(44)) D(4, [g * 2.9, .7 + .3 * p]);
  if (on(48)) D(5, [g * .07]);
  return {b: briefB(s, t), s: 0, d: WD, tag: s > 1 && t > 80 ? null : tag,
    rd: [String(240 + rnd(spd * 3) % 760), String(rnd(C[1] * 2) + 1e4).slice(1), String(g * 7919 % 9000 + 1e3)]};
}
// TWIST, the river: memories come down the corridor, turning with it, and
// fall apart into dots as they go by: HOME, the sortie number (seven
// segments), the wall's tally. Strokes: digit pairs on a 4x6 grid, two
// spaces between strokes. Then,
// as the stars open, the fleet glints far off. Without emit the segments
// go to WD (the caller's list).
const GL = ['0006  4046  0343', '0040460600', '0600234046', '40000646  0333', '1016  2026  3036  4046  0551'],
  S7 = ['0040', '4043', '4346', '0646', '0306', '0003', '0343'], SEG7 = [63, 6, 91, 79, 102, 109, 125, 7, 127, 111],
  FR = [-1.4, .8, 1.2, .6, -.3, 1.1], FL = [-30, -8, 22, -14, 8, 10, -14, 12, 38, 4];
function river(t, emit) {
  WEMIT = emit || null; BW = 0;
  // Memories at 20, 64 and 150 (after the white frame), 40 frames each;
  // pale, dim while far (the storyboard's colours).
  const q = t < 64 ? 0 : t < 150 ? 1 : 2, u = (t - [20, 64, 150][q]) / 40, mc = (u < .3 ? 0x8c71 : 0xe71a) ^ (t > 121 && t < 196 ? 0xffff : 0);
  if (u >= 0 && u < 1) {
    // Memory q's strokes into WM (a break after each).
    let n = 0, x = 0;
    const put = (w, dx) => {
      for (let j = 0; j <= w.length; j += 2) wpt(n++, w[j] > ' ' ? dx + +w[j] : NaN, +w[j + 1] || 0, 0);
    }, d = String(typeof sortie === 'number' ? sortie : 742).padStart(4, '0');
    for (let i = 0; i < 4; ++i, x += 6) {
      if (q === 1) { for (let j = 0; j < 7; ++j) if (SEG7[+d[i]] >> j & 1) put(S7[j], x); }
      else put(GL[q ? 4 : i], x);
      if (q > 1) { x = 7; break; }
    }
    const k = 2.6 / (6 * (1 - u) ** 2 + .25), a = .5 * M.sin(t * .049) + u * (q & 1 ? 1.2 : -1.2),
      c = M.cos(a) * k, sn = M.sin(a) * k, ox = FR[2 * q] * 14 - x / 2 + 1, oy = FR[2 * q + 1] * 10 - 3;
    WQ.set([c, -sn, 0, 120 + c * ox - sn * oy, sn, c, 0, 67 + sn * ox + c * oy, 0, 0, 0, 1]);
    SH.project(WQ, WM, WP, 0, n);
    // Bold: drawn again a pixel to the right.
    if (u < .62) { SH.outline(WP, 0, n, mc); WQ[3] += 1; SH.project(WQ, WM, WP, 0, n); SH.outline(WP, 0, n, mc); }
    else {
      // Crumbling: dots on the edges (two, or one for a long memory),
      // pushed out and thinned.
      const e = (u - .62) / .38, h = n > 40 ? 2 : 3;
      let m = n;
      for (let i = 0; i + 1 < n && m < 140; ++i) for (let j = 1; j < h; ++j) {
        const z = nz(7 * i + 31 * j), x0 = WP[2 * i], y0 = WP[2 * i + 1];
        if (z + .5 < e || !(x0 + WP[2 * i + 2] > -1e9)) continue;
        const X = x0 + (WP[2 * i + 2] - x0) * j / h, Y = y0 + (WP[2 * i + 3] - y0) * j / h;
        P2(m, X + (X - 120) * e * .5 + z * e * 16, Y + (Y - 67) * e * .5 + z * e * 9);
        P2(m + 1, WP[2 * m] + 1, WP[2 * m + 1]);
        P2(m + 2, NaN, 0);
        m += 3;
      }
      if (m > n + 2) SH.outline(WP, n, m - n - 1, mc);
    }
  }
  // The wall: a veil across the river comes at the camera (92..119), half
  // drawn (every other row), solid in its last 4 frames, and the white
  // frames are the hit (act2). Q0's shape, turned with the corridor.
  if (t >= 92 && t < 120) {
    const u = (t - 92) / 28, k = 14 * M.exp(2.6 * u), a = .5 * M.sin(t * .0262), c = M.cos(a) * k, sn = M.sin(a) * k;
    for (let j = 0; j < 4; ++j) P2(j, 120 + c * Q0[j][0] - sn * Q0[j][1], 67 + sn * Q0[j][0] + c * Q0[j][1]);
    SH.fill(WP, [0, 4], undefined, 0xde5f, t < 116 ? {step: 2} : undefined);
    SH.outline(WP, 0, 4, WH, {closed: true});
  }
  // The fleet, pink, as the stars open.
  if (t >= 216) {
    let n = 0;
    for (let j = 0; j < 30; ++j) {
      const i = j / 6 | 0, v = 1 + (t - 216) * .03, r = (1.5 + (t - 216) * .06) * [0, 1, 0, -1, 0][j % 6 % 5];
      P2(n++, j % 6 > 4 || (t + 3 * i) % 6 > 3 ? NaN : 120 + FL[2 * i] * v + r, 67 + FL[2 * i + 1] * v + (j % 6 & 1 ? 0 : (j % 6 & 2 ? 1.6 : -1.6) * (1.5 + (t - 216) * .06)));
    }
    SH.outline(WP, 0, 29, 0xfcda);
  }
}
