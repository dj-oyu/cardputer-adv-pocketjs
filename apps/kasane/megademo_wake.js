// MEGADEMO chunk 3: WAKE, the prologue (Tama in the dark). play() calls
// wake(); it draws with pocket.shapes (docs/api/shapes.md).
'use strict';
// Tama peering in, backlit in the dark, in close-up: the head is bigger
// than the view, so only the eyes, the ears, the whiskers and part of the
// outline show. The backdrop is the silhouette and the lit room behind it
// is the fill (the glow minus the head and ears). A cyan rim on the outer
// edges, magenta inner ears, glowing eyes. The head is 3D points on an
// ellipsoid, turned and seen in perspective: a turn slides the eyes across
// and folds the ears (an ear edge-on hides its inside), leaning in swells
// the near parts; the outline is worked out for the turn.
// POSE: 11 keys of [t, scale, yaw, pitch, roll, gaze, eye open, ear
// spread, camera distance, x, y]. Peering from the side; noticing you
// wake; facing you; a slow blink (a cat's "all is well"); a tilt; leaning
// in. At WCUT the viewer has blinked and sat up: the room, the TV off.
const WCUT = 74;
const POSE = [0, 1.9, .5, .05, -.38, -1, 1, 0, 260, 60, -16, 17, 1.9, .5, .05, -.38, -1, 1, 0, 260, 56, -14,
  24, 1.85, .42, 0, -.32, -.6, 1, 0, 260, 50, 4, 36, 1.6, .06, -.06, -.1, 0, 1, 0, 240, 6, 18,
  41, 1.6, .04, -.06, -.1, 0, 1, .5, 240, 6, 18, 45, 1.6, .04, -.04, -.1, 0, .06, 1, 240, 6, 18,
  49, 1.6, .04, -.04, -.1, 0, .06, 1, 240, 6, 18, 54, 1.6, .06, -.08, .2, .3, .75, .6, 240, 0, 18,
  60, 1.6, .06, -.1, .24, .3, .75, .4, 240, 0, 18, 68, 2.1, .1, -.2, .08, 0, .85, 0, 150, 0, 14,
  74, 2.1, .1, -.2, .08, 0, .85, 0, 150, 0, 14];
// The back light: an elliptical glow behind the head's top left (cx, cy,
// rx, ry); the rest of the room stays as dark as the cat.
const BACK = [124, 32, 88, 34];
// The head: an ellipsoid (radii 52, 44, 46). Ears (x, y, z each): base out,
// outer edge, tip, inner edge, base in (the left one; the right mirrors
// it). Whiskers: root x y, tip x y.
const EAR = [-46, -22, 8, -52, -48, 2, -44, -74, -4, -25, -60, 8, -14, -40, 18],
  WHISK = [10, 16, 78, 4, 12, 20, 80, 18, 11, 24, 72, 32];
// WAKE's points: JS builds a frame's model points (x right, y down, z to the
// viewer) into WM; shapes.project puts them on the screen in WP (same
// index) and shapes.fill / outline draw them, in C (the same geometry in
// JS cost ~130 ms a frame on the device, docs/apps/megademo-shapes.md).
// Ranges: head 0 (28), ears 28 (5 each), their insides 38 (10 each), the
// glow's core 58 (16, screen points, set each frame), whiskers 74 (2
// each), eyes 86 (30 each: iris 12, pupil 12, the shut lid 4, the
// highlight 2). The briefing and TWIST (megademo_brief.js) draw with the
// same arrays. QQ: this frame's pose; EP: an ear's 5 points.
const SH = pocket.shapes, WM = new Float32Array(3 * 146), WP = new Float32Array(2 * 146), WQ = new Float32Array(12);
const QQ = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0], EP = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
const W_EAR = 28, W_CORE = 58, W_WH = 74, W_EYE = 86;
// Colours: the lit room 0x218b, the glowing iris 0xbf9f, the magenta inner
// ear 0x68aa, the cyan rim C_RIM.
const C_RIM = 0x067f;
// Polygons as [start, count] pairs, and the options of each draw.
const W_CUT = [0, 28, W_EAR, 5, W_EAR + 5, 5], W_GLOW = [W_CORE, 16],
  O_CORE = {top: BACK[1] - BACK[3] * .68, bottom: BACK[1] + BACK[3] * .68},
  W_RINGH = W_CUT.concat(W_GLOW), O_RING = {step: 2}, O_HEAD = {closed: true, hidden: W_CUT.slice(2)},
  O_EAR = {pieces: 2, hidden: [0, 28]}, W_INS = [[38, 10], [48, 10]],
  W_IRIS = [[W_EYE, 12], [W_EYE + 30, 12]], W_PUP = [[W_EYE + 12, 12], [W_EYE + 42, 12]];
const wpt = (i, x, y, z) => { WM[3 * i] = x; WM[3 * i + 1] = y; WM[3 * i + 2] = z; };
// A point of eye k's almond (sg its side): on the face, z on the
// ellipsoid's front, slanted.
function eye(i, sg, x, y) {
  const X = sg * 21 + x, Y = -7 + y - sg * x * .18;
  wpt(i, X, Y, 46 * M.sqrt(M.max(0, 1 - (X / 52) ** 2 - (Y / 44) ** 2)) + 1);
}
function wake(t) {
  BW = 0;
  if (t >= WCUT) return BG(0);
  BG(0x0043);
  // The glow's core is set each frame: the briefing writes over WP.
  for (let j = 0; j < 16; ++j) {
    WP[2 * (W_CORE + j)] = BACK[0] + BACK[2] * .68 * M.cos(j * PI / 8);
    WP[2 * (W_CORE + j) + 1] = BACK[1] + BACK[3] * .68 * M.sin(j * PI / 8);
  }
  let a = 0;
  while (a < 99 && POSE[a + 11] <= t) a += 11;
  const u = smooth(ramp(t, POSE[a], POSE[a + 11]));
  for (let j = 0; j < 11; ++j) QQ[j] = POSE[a + j] + (POSE[a + 11 + j] - POSE[a + j]) * u;
  // Breathing, and a slow sway.
  const S = QQ[1] * (1 + .012 * M.sin(t * .2)), cy = M.cos(QQ[2]), sy = M.sin(QQ[2]), cp = M.cos(QQ[3]),
    sp = M.sin(QQ[3]), r = QQ[4] + .03 * M.sin(t * .11), c = M.cos(r), sn = M.sin(r);
  // Head space to the screen as one projective matrix (shapes.project):
  // turn (yaw), nod (pitch), the camera D in front (nearer is bigger, x
  // and y over w = D - z), then roll and scale about (X0, Y0). Rows of the
  // rotation: (cy, 0, sy), (sp sy, cp, -sp cy); the view w = (cp sy, -sp, -cp cy).
  {
    const D = QQ[8], K = D * S, X = 120 + QQ[9], Y = 100 + QQ[10] + 1.5 * M.sin(t * .2),
      a1 = sp * sy, b = -sp * cy, e = cp * sy, l = -sp, s = -cp * cy;
    WQ[0] = X * e + K * (c * cy - sn * a1); WQ[1] = X * l + K * -(sn * cp); WQ[2] = X * s + K * (c * sy - sn * b);
    WQ[4] = Y * e + K * (sn * cy + c * a1); WQ[5] = Y * l + K * (c * cp); WQ[6] = Y * s + K * (sn * sy + c * b);
    WQ[8] = e; WQ[9] = l; WQ[10] = s; WQ[3] = X * D; WQ[7] = Y * D; WQ[11] = D;
  }
  // The outline: the view direction in head space scaled by the radii (w),
  // and the radii times the unit circle across it (e1, e2). Fur: the
  // cheeks tufted (every other point out), the jaw fuller.
  {
    let e = -sy * cp / 52, l = sp / 44, s = cy * cp / 46;
    const L = M.hypot(e, l, s);
    e /= L; l /= L; s /= L;
    const O = M.hypot(s, -e), e0 = s / O, T = -e / O, f0 = l * T, f1 = s * e0 - e * T, f2 = -l * e0;
    for (let k = 0; k < 28; ++k) {
      const g = k * PI / 14, G = M.cos(g), sg = M.sin(g), x = 52 * (G * e0 + sg * f0), y = 44 * (sg * f1),
        z = 46 * (G * T + sg * f2), f = y > 0 && M.abs(x) > 30 ? (k & 1 ? 1.13 : 1.02) : y > 20 ? 1.05 : 1;
      wpt(k, x * f, y * f, z * f);
    }
  }
  // Ears: spread out when at ease (QQ[7]); the near ear flicks as you
  // wake, the far one in the tilt. Each is a plane: facing away, its
  // inside hides. The inside: the ear drawn in towards its middle, its
  // corners cut once (Chaikin) so it reads as a soft hollow.
  for (let k = 0; k < 2; ++k) {
    const sg = k ? -1 : 1, fl = k ? t >= 56 && t < 59 : t >= 21 && t < 24;
    for (let j = 0; j < 5; ++j) {
      const w = j && j < 4 ? 1 : 0, H = j === 2 ? 1 : .5;
      let x = EAR[3 * j] - sg * 7 * QQ[7] * w * H, z = EAR[3 * j + 2];
      const y = EAR[3 * j + 1] + 4 * QQ[7] * w * H;
      if (fl && w) { x -= sg * 8 * H; z -= 14 * H; }
      EP[3 * j] = x * sg; EP[3 * j + 1] = y; EP[3 * j + 2] = z;
      wpt(W_EAR + 5 * k + j, x * sg, y, z);
    }
    for (let j = 0; j < 5; ++j) {
      const i = 3 * j, n = 3 * ((j + 1) % 5), f = j === 2 ? .38 : j === 0 || j === 4 ? .6 : .5,
        g = n === 6 ? .38 : n === 0 || n === 12 ? .6 : .5;
      for (let h = 0; h < 2; ++h) {
        const v = h ? .75 : .25, o = 3 * (38 + 10 * k + 2 * j + h);
        for (let d = 0; d < 3; ++d) {
          const m = (EP[d] + EP[12 + d] + EP[6 + d] * .6) / 2.6, A = EP[i + d] + (m - EP[i + d]) * f,
            B = EP[n + d] + (m - EP[n + d]) * g;
          WM[o + d] = A + (B - A) * v + (d > 1 ? 1 : 0);
        }
      }
    }
  }
  // Whiskers catch the back light past the cheeks; they twitch.
  for (let k = 0; k < 6; ++k) {
    const w = 4 * (k % 3), sg = k < 3 ? -1 : 1;
    wpt(W_WH + 2 * k, sg * (WHISK[w] + WHISK[w + 2]) / 2, (WHISK[w + 1] + WHISK[w + 3]) / 2, 22);
    wpt(W_WH + 2 * k + 1, sg * WHISK[w + 2], WHISK[w + 3] + 2 * M.sin(t * .3 + k), 14);
  }
  // Eyes (almonds, slanted): wide in the dark, the pupils big; the far eye
  // narrows with the turn by itself. Shut: a line.
  const op = QQ[6], h = 8 * op, px = QQ[5] * 4.4, q = t < 40 ? 4.4 : 3, ph = h * .92;
  for (let k = 0; k < 2; ++k) {
    const sg = k ? 1 : -1, o = W_EYE + 30 * k;
    for (let j = 0; j < 12; ++j) {
      const g = j * PI / 6, G = M.cos(g), R = M.sin(g);
      eye(o + j, sg, 11 * G, h * R * (1 - .3 * G * G));
      eye(o + 12 + j, sg, px + q * G, ph * R);
    }
    eye(o + 24, sg, -11, 0); eye(o + 25, sg, -4, 2); eye(o + 26, sg, 4, 2); eye(o + 27, sg, 11, 0);
    eye(o + 28, sg, px - q * .8, -h * .45); eye(o + 29, sg, px - q * .2, -h * .45);
  }
  SH.project(WQ, WM, WP, 0, W_CORE);
  SH.project(WQ, WM, WP, W_WH, 146 - W_WH);
  // The lit room: the glow minus the head and the ears, soft at its edge:
  // the core (.68 of it) every row, the ring outside every other row.
  SH.fill(WP, W_GLOW, W_CUT, 0x218b, O_CORE);
  SH.fill(WP, BACK, W_RINGH, 0x218b, O_RING);
  // The rim on the outer edges only: a head edge inside an ear is not lit,
  // nor the halves of an ear's edges inside the head.
  SH.outline(WP, 0, 28, C_RIM, O_HEAD);
  SH.outline(WP, W_EAR, 5, C_RIM, O_EAR);
  SH.outline(WP, W_EAR + 5, 5, C_RIM, O_EAR);
  // An ear's inside shows while the ear faces the viewer (the sign of its
  // projected winding).
  for (let k = 0; k < 2; ++k) {
    const a = 2 * (W_EAR + 5 * k), C = ((WP[a + 4] - WP[a]) * (WP[a + 9] - WP[a + 1]) - (WP[a + 5] - WP[a + 1]) * (WP[a + 8] - WP[a])) * (k ? -1 : 1);
    if (C > 400) SH.fill(WP, W_INS[k], undefined, 0x68aa);
  }
  for (let k = 0; k < 6; ++k) SH.outline(WP, W_WH + 2 * k, 2, C_RIM);
  // The dark pupils are the backdrop: only the glowing iris round them is drawn.
  for (let k = 0; k < 2; ++k) {
    const o = W_EYE + 30 * k;
    if (op < .2) { SH.outline(WP, o + 24, 4, C_RIM); continue; }
    SH.fill(WP, W_IRIS[k], W_PUP[k], 0xbf9f);
    SH.outline(WP, o + 28, 2, WH);
  }
}
