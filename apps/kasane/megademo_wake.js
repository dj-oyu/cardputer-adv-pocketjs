// MEGADEMO chunk 3: WAKE, the prologue (Tama in the dark). act2's scene
// table calls wake(); it draws with pocket.shapes (docs/api/shapes.md).
'use strict';
// WAKE: Tama peering in, backlit in the dark, in close-up: the head is
// bigger than the view, so only the eyes, the ears, the whiskers and part
// of the outline show (never the whole shape). The backdrop is the
// silhouette and the lit room behind it is the fill (the glow minus the
// head and ears).
// A cyan rim on the outer edges, magenta inner ears (the icon's cyber
// coat), glowing eyes. The head is 3D points on an ellipsoid, turned and
// seen in perspective: a turn slides the eyes across and folds the ears
// (an ear edge-on hides its inside), leaning in swells the near parts; the
// outline is worked out for the turn (cheek tufts give it corners).
// POSE keys: [t, scale, yaw, pitch, roll, gaze, eye open, ear spread,
// camera distance, x, y]. Peering from the side; noticing you wake; facing
// you; a slow blink (a cat's "all is well"); a tilt; leaning in. At WCUT
// the viewer has blinked and sat up: the room, and the TV is off.
const WCUT = 74;
const POSE = [[0, 1.9, .5, .05, -.38, -1, 1, 0, 260, 60, -16], [17, 1.9, .5, .05, -.38, -1, 1, 0, 260, 56, -14],
  [24, 1.85, .42, 0, -.32, -.6, 1, 0, 260, 50, 4], [36, 1.6, .06, -.06, -.1, 0, 1, 0, 240, 6, 18],
  [41, 1.6, .04, -.06, -.1, 0, 1, .5, 240, 6, 18], [45, 1.6, .04, -.04, -.1, 0, .06, 1, 240, 6, 18],
  [49, 1.6, .04, -.04, -.1, 0, .06, 1, 240, 6, 18], [54, 1.6, .06, -.08, .2, .3, .75, .6, 240, 0, 18],
  [60, 1.6, .06, -.1, .24, .3, .75, .4, 240, 0, 18], [68, 2.1, .1, -.2, .08, 0, .85, 0, 150, 0, 14],
  [74, 2.1, .1, -.2, .08, 0, .85, 0, 150, 0, 14]];
// The back light: an elliptical glow behind the head's top left; the rest
// of the room stays as dark as the cat, outlined only by its rim.
const BACK = [124, 32, 88, 34];
// The head: an ellipsoid (radii WRAD); its outline is worked out for the
// turn (the points whose normal is across the view), furred at the
// cheeks. Ears: base out, outer edge, tip, inner edge, base in (the left
// one; the right mirrors it). Whiskers: root, tip.
const WRAD = [52, 44, 46], EAR = [[-46, -22, 8], [-52, -48, 2], [-44, -74, -4], [-25, -60, 8], [-14, -40, 18]],
  WHISK = [[10, 16, 78, 4], [12, 20, 80, 18], [11, 24, 72, 32]];
// WAKE's points: JS builds a frame's model points (x right, y down, z to the
// viewer) into WM; shapes.project puts them on the screen in WP (same
// index) and shapes.fill / outline draw them into the frame, in C (the same
// geometry in JS cost ~130 ms a frame on the device,
// docs/apps/megademo-shapes.md). Ranges: head 0 (28), ears 28 (5 each),
// their insides 38 (10 each), the glow's core 58 (16, screen points, set
// once), whiskers 74 (2 each), eyes 86 (30 each: iris 12, pupil 12, the shut
// lid 4, the highlight 2).
const SH = pocket.shapes, WN = 146, WM = new Float32Array(3 * WN), WP = new Float32Array(2 * WN), WQ = new Float32Array(12);
const W_EAR = 28, W_IN = 38, W_CORE = 58, W_WH = 74, W_EYE = 86;
for (let j = 0; j < 16; ++j) {
  WP[2 * (W_CORE + j)] = BACK[0] + BACK[2] * .68 * M.cos(j * M.PI / 8);
  WP[2 * (W_CORE + j) + 1] = BACK[1] + BACK[3] * .68 * M.sin(j * M.PI / 8);
}
// Colours: the lit room, the glowing iris, the magenta inner ear, the cyan
// rim, the eye's highlight.
const C_GLOW = 0x218b, C_IRIS = 0xbf9f, C_EAR = 0x68aa, C_RIM = 0x067f;
// Polygons as [start, count] pairs, and the options of each draw.
const W_CUT = [0, 28, W_EAR, 5, W_EAR + 5, 5], W_RING = [BACK[0], BACK[1], BACK[2], BACK[3]],
  O_CORE = {top: BACK[1] - BACK[3] * .68, bottom: BACK[1] + BACK[3] * .68},
  W_RINGH = W_CUT.concat([W_CORE, 16]), O_RING = {step: 2}, O_HEAD = {closed: true, hidden: W_CUT.slice(2)},
  O_EAR = {pieces: 2, hidden: [0, 28]}, W_INS = [[W_IN, 10], [W_IN + 10, 10]],
  W_IRIS = [[W_EYE, 12], [W_EYE + 30, 12]], W_PUP = [[W_EYE + 12, 12], [W_EYE + 42, 12]];
const wpt = (i, x, y, z) => { WM[3 * i] = x; WM[3 * i + 1] = y; WM[3 * i + 2] = z; };
// A point on the face (z on the ellipsoid's front).
const wsurf = (i, x, y, d) => wpt(i, x, y, WRAD[2] * M.sqrt(M.max(0, 1 - (x / WRAD[0]) ** 2 - (y / WRAD[1]) ** 2)) + d);
// Without emit (the host tests) the drawing is listed: the harness's shapes
// put each segment in WD.
let WD = null, WEMIT = null;
// WAKE's backdrop: the silhouette, until the TV is off.
const wakeB = t => t < WCUT ? 0x0043 : 0;
function wake(t, emit) {
  WD = []; WEMIT = emit || null;
  if (t >= WCUT) return {b: 0, s: 0, d: WD};
  let i = 0;
  while (i < POSE.length - 2 && POSE[i + 1][0] <= t) ++i;
  const A = POSE[i], B = POSE[i + 1], u = smooth(ramp(t, A[0], B[0])), q = A.map((v, j) => v + (B[j] - v) * u);
  // Breathing, and a slow sway.
  const S = q[1] * (1 + .012 * M.sin(t * .2)), cy = M.cos(q[2]), sy = M.sin(q[2]), cp = M.cos(q[3]), sp = M.sin(q[3]),
    c = M.cos(q[4] + .03 * M.sin(t * .11)), sn = M.sin(q[4] + .03 * M.sin(t * .11));
  // Head space to the screen as one projective matrix (shapes.project):
  // turn (yaw), nod (pitch), the camera D in front (nearer is bigger, x
  // and y over w = D - z), then roll and scale about (X0, Y0).
  {
    const D = q[8], K = D * S, X0 = 120 + q[9], Y0 = 100 + q[10] + 1.5 * M.sin(t * .2),
      R0 = [cy, 0, sy], R1 = [sp * sy, cp, -sp * cy], W = [cp * sy, -sp, -cp * cy];
    for (let j = 0; j < 3; ++j) {
      WQ[j] = X0 * W[j] + K * (c * R0[j] - sn * R1[j]);
      WQ[4 + j] = Y0 * W[j] + K * (sn * R0[j] + c * R1[j]);
      WQ[8 + j] = W[j];
    }
    WQ[3] = X0 * D; WQ[7] = Y0 * D; WQ[11] = D;
  }
  // The view direction in head space, scaled by the radii (w): the
  // outline is the radii times the unit circle across w (e1, e2).
  const w = [-sy * cp / 52, sp / 44, cy * cp / 46], wl = M.hypot(w[0], w[1], w[2]);
  w[0] /= wl; w[1] /= wl; w[2] /= wl;
  const e1 = [w[2], 0, -w[0]], l1 = M.hypot(e1[0], e1[2]);
  e1[0] /= l1; e1[2] /= l1;
  const e2 = [w[1] * e1[2], w[2] * e1[0] - w[0] * e1[2], -w[1] * e1[0]];
  for (let k = 0; k < 28; ++k) {
    const g = k * M.PI / 14, cg = M.cos(g), sg = M.sin(g);
    let x = 52 * (cg * e1[0] + sg * e2[0]), y = 44 * (cg * e1[1] + sg * e2[1]), z = 46 * (cg * e1[2] + sg * e2[2]);
    // Fur: the cheeks tufted (every other point out), the jaw fuller.
    const f = y > 0 && M.abs(x) > 30 ? (k & 1 ? 1.13 : 1.02) : y > 20 ? 1.05 : 1;
    wpt(k, x * f, y * f, z * f);
  }
  // Ears: spread out when at ease; the near ear flicks as you wake, the
  // far one in the tilt. Each is a plane: facing away, its inside hides.
  for (let k = 0; k < 2; ++k) {
    const sg = k ? -1 : 1, fl = k ? t >= 56 && t < 59 : t >= 21 && t < 24, P = [];
    for (let j = 0; j < 5; ++j) {
      const e = EAR[j], w = j && j < 4 ? 1 : 0, tip = j === 2 ? 1 : .5;
      let x = e[0] - sg * 7 * q[7] * w * tip, y = e[1] + 4 * q[7] * w * tip, z = e[2];
      if (fl && w) { x -= sg * 8 * tip; z -= 14 * tip; }
      P.push([x * sg, y, z]);
      wpt(W_EAR + 5 * k + j, x * sg, y, z);
    }
    const m = [0, 1, 2].map(a => (P[0][a] + P[4][a] + P[2][a] * .6) / 2.6);
    // The inside: the ear drawn in towards its middle, its corners cut
    // once (Chaikin) so it reads as a soft hollow, not a triangle.
    const N = P.map((p, j) => {
      const f = j === 2 ? .38 : j === 0 || j === 4 ? .6 : .5;
      return [0, 1, 2].map(d => p[d] + (m[d] - p[d]) * f);
    });
    for (let j = 0; j < 5; ++j) {
      const a = N[j], b = N[(j + 1) % 5];
      for (let h = 0; h < 2; ++h) {
        const v = h ? .75 : .25;
        wpt(W_IN + 10 * k + 2 * j + h, a[0] + (b[0] - a[0]) * v, a[1] + (b[1] - a[1]) * v, a[2] + (b[2] - a[2]) * v + 1);
      }
    }
  }
  // Whiskers catch the back light past the cheeks; they twitch.
  for (let k = 0; k < 6; ++k) {
    const w = WHISK[k % 3], sg = k < 3 ? -1 : 1;
    wpt(W_WH + 2 * k, sg * (w[0] + w[2]) / 2, (w[1] + w[3]) / 2, 22);
    wpt(W_WH + 2 * k + 1, sg * w[2], w[3] + 2 * M.sin(t * .3 + k), 14);
  }
  // Eyes (almonds on the face, slanted): wide in the dark, the pupils big;
  // the far eye narrows with the turn by itself. Shut: a line.
  const op = q[6], h = 8 * op, px = q[5] * 4.4, pw = t < 40 ? 4.4 : 3, ph = h * .92;
  for (let k = 0; k < 2; ++k) {
    const sg = k ? 1 : -1, o = W_EYE + 30 * k, E = (i, x, y) => wsurf(o + i, sg * 21 + x, -7 + y - sg * x * .18, 1);
    for (let j = 0; j < 12; ++j) {
      const g = j * M.PI / 6, cg = M.cos(g), s1 = M.sin(g);
      E(j, 11 * cg, h * s1 * (1 - .3 * cg * cg));
      E(12 + j, px + pw * cg, ph * s1);
    }
    E(24, -11, 0); E(25, -4, 2); E(26, 4, 2); E(27, 11, 0);
    E(28, px - pw * .8, -h * .45); E(29, px - pw * .2, -h * .45);
  }
  SH.project(WQ, WM, WP, 0, W_CORE);
  SH.project(WQ, WM, WP, W_WH, WN - W_WH);
  // The lit room: the glow minus the head and the ears, soft at its edge:
  // the core (.68 of it) every row, the ring outside every other row.
  SH.fill(WP, [W_CORE, 16], W_CUT, C_GLOW, O_CORE);
  SH.fill(WP, W_RING, W_RINGH, C_GLOW, O_RING);
  // The rim on the outer edges only: a head edge inside an ear is not lit,
  // nor the halves of an ear's edges inside the head.
  SH.outline(WP, 0, 28, C_RIM, O_HEAD);
  SH.outline(WP, W_EAR, 5, C_RIM, O_EAR);
  SH.outline(WP, W_EAR + 5, 5, C_RIM, O_EAR);
  // An ear's inside shows while the ear faces the viewer (the sign of its
  // projected winding).
  for (let k = 0; k < 2; ++k) {
    const a = 2 * (W_EAR + 5 * k), fc = ((WP[a + 4] - WP[a]) * (WP[a + 9] - WP[a + 1]) - (WP[a + 5] - WP[a + 1]) * (WP[a + 8] - WP[a])) * (k ? -1 : 1);
    if (fc > 400) SH.fill(WP, W_INS[k], undefined, C_EAR);
  }
  for (let k = 0; k < 6; ++k) SH.outline(WP, W_WH + 2 * k, 2, C_RIM);
  // The dark pupils are the backdrop: only the glowing iris around them is
  // drawn.
  for (let k = 0; k < 2; ++k) {
    const o = W_EYE + 30 * k;
    if (op < .2) { SH.outline(WP, o + 24, 4, C_RIM); continue; }
    SH.fill(WP, W_IRIS[k], W_PUP[k], C_IRIS);
    SH.outline(WP, o + 28, 2, WH);
  }
  return {b: wakeB(t), s: 0, d: WD};
}
