// MEGADEMO chunk 3: WAKE, the prologue (Tama in the dark). act2's scene
// table calls wake(); the frame loop draws it through emit.
'use strict';
// WAKE: Tama peering in, backlit in the dark, in close-up: the head is
// bigger than the view, so only the eyes, the ears, the whiskers and part
// of the outline show (never the whole shape). The backdrop is the
// silhouette and the lit room behind it is the fill (the glow minus the
// head and ears, under the 8192 raster steps a frame), in tz trapezoids.
// A cyan rim on the outer edges, magenta inner ears (the icon's cyber
// coat), glowing eyes. The head is 3D points on an ellipsoid, turned and
// seen in perspective: a turn slides the eyes across and folds the ears
// (an ear edge-on hides its inside), leaning in swells the near parts; the
// outline is the hull of the turned points (cheek tufts give it corners).
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
// A point on the face (z on the ellipsoid's front).
const wsurf = (x, y, d) => [x, y, WRAD[2] * M.sqrt(M.max(0, 1 - (x / WRAD[0]) ** 2 - (y / WRAD[1]) ** 2)) + d];
// WAKE draws ~150 times a frame: on the device each draw goes straight to
// the loop's emit(plan, inputs) with one reused inputs array (a list of
// fresh arrays held ~25 KB of guest heap and ran it out between
// collections). Without emit (the host tests) it returns the list. The
// spans and gaps are reused buffers too.
const WR = [0, 0], WHS = [[0, 0], [0, 0], [0, 0], [0, 0]], WG = [[], [], []], WTA = [0, 0, 0, 0, 0, 0], WLA = [0, 0, 0, 0];
let WD = null, WEMIT = null;
function wtz(k, x0, x1, y, d0, d1, n) {
  const v = WEMIT ? WTA : [0, 0, 0, 0, 0, 0];
  v[0] = x0; v[1] = x1; v[2] = y; v[3] = d0; v[4] = d1; v[5] = n;
  if (WEMIT) WEMIT(k, v); else WD.push([k, v]);
}
function wln(k, a, b) {
  if (a[0] > 239 && b[0] > 239 || a[1] < 0 && b[1] < 0 || a[0] < 0 && b[0] < 0 || a[1] > 134 && b[1] > 134) return;
  const v = WEMIT ? WLA : [0, 0, 0, 0];
  v[0] = a[0]; v[1] = a[1]; v[2] = b[0]; v[3] = b[1];
  if (WEMIT) WEMIT(k, v); else WD.push([k, v]);
}
// WAKE's backdrop: the silhouette, until the TV is off.
const wakeB = t => t < WCUT ? 0x0043 : 0;
// The x span of a convex polygon on row y into WR; false if none.
function wspan(Q, y) {
  let l = 1e9, r = -1e9;
  for (let k = 0; k < Q.length; ++k) {
    const a = Q[k], b = Q[(k + 1) % Q.length];
    if ((a[1] - y) * (b[1] - y) <= 0 && a[1] !== b[1]) {
      const x = a[0] + (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]);
      if (x < l) l = x;
      if (x > r) r = x;
    }
  }
  WR[0] = l; WR[1] = r;
  return l <= r;
}
// Row y of [l, r] (clipped to the screen) less the holes' spans, as flat
// x0, x1 pairs into g; returns the pair count.
function wminus(l, r, holes, y, g) {
  let n = 0;
  for (let k = 0; k < holes.length; ++k) if (wspan(holes[k], y)) {
    let j = n++;
    while (j && WHS[j - 1][0] > WR[0]) { WHS[j][0] = WHS[j - 1][0]; WHS[j][1] = WHS[j - 1][1]; --j; }
    WHS[j][0] = WR[0]; WHS[j][1] = WR[1];
  }
  g.length = 0;
  let x = l < 0 ? 0 : l;
  const e = r > 239 ? 239 : r;
  for (let k = 0; k < n; ++k) {
    if (WHS[k][0] > x) g.push(x, WHS[k][0] < e ? WHS[k][0] : e);
    if (WHS[k][1] > x) x = WHS[k][1];
  }
  if (x < e) g.push(x, e);
  let m = 0;
  for (let k = 0; k < g.length; k += 2) if (g[k] < g[k + 1]) { g[m++] = g[k]; g[m++] = g[k + 1]; }
  g.length = m;
  return m / 2;
}
// Rows y0..y1 of rows(y, g) in strips of 4 rows (one trapezoid a gap);
// where the gap count changes in a strip, row by row. Plan k draws a line
// every s rows (1, or 2 for the glow's dithered ring).
function wstrips(y0, y1, rows, k, s) {
  y0 = M.max(0, M.ceil(y0)); y1 = M.min(134, M.floor(y1));
  if (s > 1) y0 += y0 & 1;
  for (let y = y0; y < y1; y += 4) {
    const n = M.min(4, y1 - y), a = WG[0], b = WG[1], na = rows(y, a), nb = rows(y + n, b), m = n / s;
    if (n < s) break;
    if (na === nb) for (let j = 0; j < 2 * na; j += 2) wtz(k, a[j], a[j + 1], y, (b[j] - a[j]) / m, (b[j + 1] - a[j + 1]) / m, m);
    else for (let r = 0; r < n; r += s) {
      const c = WG[2], nc = rows(y + r, c);
      for (let j = 0; j < 2 * nc; j += 2) wtz(k, c[j], c[j + 1], y + r, 0, 0, 1);
    }
  }
}
function winside(Q, p) {
  let pos = 0, neg = 0;
  for (let k = 0; k < Q.length; ++k) {
    const a = Q[k], b = Q[(k + 1) % Q.length], x = (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]);
    if (x > 0) ++pos; else if (x < 0) ++neg;
  }
  return !pos || !neg;
}
const WCORE = [];
for (let j = 0; j < 16; ++j) WCORE.push([BACK[0] + BACK[2] * .68 * M.cos(j * M.PI / 8), BACK[1] + BACK[3] * .68 * M.sin(j * M.PI / 8)]);
function wake(t, emit) {
  WD = []; WEMIT = emit || null;
  if (t >= WCUT) return {b: 0, s: 0, d: WD};
  let i = 0;
  while (i < POSE.length - 2 && POSE[i + 1][0] <= t) ++i;
  const A = POSE[i], B = POSE[i + 1], u = smooth(ramp(t, A[0], B[0])), q = A.map((v, j) => v + (B[j] - v) * u);
  // Breathing, and a slow sway.
  const S = q[1] * (1 + .012 * M.sin(t * .2)), cy = M.cos(q[2]), sy = M.sin(q[2]), cp = M.cos(q[3]), sp = M.sin(q[3]),
    c = M.cos(q[4] + .03 * M.sin(t * .11)), sn = M.sin(q[4] + .03 * M.sin(t * .11)), D = q[8],
    X0 = 120 + q[9], Y0 = 100 + q[10] + 1.5 * M.sin(t * .2);
  // Head space (x right, y down, z to the viewer) to the screen: turn,
  // nod, perspective (nearer is bigger), roll.
  const tf = (x, y, z) => {
    const x1 = x * cy + z * sy, z1 = z * cy - x * sy, y2 = y * cp - z1 * sp, z2 = y * sp + z1 * cp, k = D / (D - z2) * S;
    return [X0 + (x1 * c - y2 * sn) * k, Y0 + (x1 * sn + y2 * c) * k, z2];
  };
  // The view direction in head space, scaled by the radii (w): the
  // outline is the radii times the unit circle across w (e1, e2).
  const w = [-sy * cp / 52, sp / 44, cy * cp / 46], wl = M.hypot(w[0], w[1], w[2]);
  w[0] /= wl; w[1] /= wl; w[2] /= wl;
  const e1 = [w[2], 0, -w[0]], l1 = M.hypot(e1[0], e1[2]);
  e1[0] /= l1; e1[2] /= l1;
  const e2 = [w[1] * e1[2], w[2] * e1[0] - w[0] * e1[2], -w[1] * e1[0]], head = [];
  for (let k = 0; k < 28; ++k) {
    const g = k * M.PI / 14, cg = M.cos(g), sg = M.sin(g);
    let x = 52 * (cg * e1[0] + sg * e2[0]), y = 44 * (cg * e1[1] + sg * e2[1]), z = 46 * (cg * e1[2] + sg * e2[2]);
    // Fur: the cheeks tufted (every other point out), the jaw fuller.
    const f = y > 0 && M.abs(x) > 30 ? (k & 1 ? 1.13 : 1.02) : y > 20 ? 1.05 : 1;
    head.push(tf(x * f, y * f, z * f));
  }
  // Ears: spread out when at ease; the near ear flicks as you wake, the
  // far one in the tilt. Each is a plane: facing away, its inside hides.
  const ear = [], inner = [];
  for (let k = 0; k < 2; ++k) {
    const sg = k ? -1 : 1, fl = k ? t >= 56 && t < 59 : t >= 21 && t < 24, P = [], Q = [];
    for (let j = 0; j < 5; ++j) {
      const e = EAR[j], w = j && j < 4 ? 1 : 0, tip = j === 2 ? 1 : .5;
      let x = e[0] - sg * 7 * q[7] * w * tip, y = e[1] + 4 * q[7] * w * tip, z = e[2];
      if (fl && w) { x -= sg * 8 * tip; z -= 14 * tip; }
      P.push([x * sg, y, z]);
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
      for (const v of [.25, .75]) Q.push(tf(a[0] + (b[0] - a[0]) * v, a[1] + (b[1] - a[1]) * v, a[2] + (b[2] - a[2]) * v + 1));
    }
    const e = P.map(p => tf(p[0], p[1], p[2]));
    // Facing: the sign of the projected inside's winding.
    const a = e[0], b = e[2], d = e[4], fc = ((b[0] - a[0]) * (d[1] - a[1]) - (b[1] - a[1]) * (d[0] - a[0])) * sg;
    ear.push(e); if (fc > 400) inner.push(Q);
  }
  const cut = [head, ear[0], ear[1]];
  // The lit room: the glow minus the head and the ears, soft at its edge:
  // the core (.68 of it) every row, the ring outside every other row.
  wstrips(BACK[1] - BACK[3] * .68, BACK[1] + BACK[3] * .68, (y, g) => wspan(WCORE, y) ? wminus(WR[0], WR[1], cut, y, g) : (g.length = 0), 0, 1);
  cut.push(WCORE);
  wstrips(BACK[1] - BACK[3], BACK[1] + BACK[3], (y, g) => {
    const v = (y - BACK[1]) / BACK[3], w = BACK[2] * M.sqrt(M.max(0, 1 - v * v));
    return w > 0 ? wminus(BACK[0] - w, BACK[0] + w, cut, y, g) : (g.length = 0);
  }, 6, 2);
  // A convex shape filled, less any holes (the dark pupils are the backdrop:
  // only the glowing iris around them is drawn).
  const fill = (Q, k, holes) => {
    let y0 = 1e9, y1 = -1e9;
    for (const p of Q) { y0 = M.min(y0, p[1]); y1 = M.max(y1, p[1]); }
    wstrips(y0 + .01, y1 - .01, (y, g) => wspan(Q, y) ? wminus(WR[0], WR[1], holes, y, g) : (g.length = 0), k, 1);
  };
  // The rim on the outer edges only: a head edge inside an ear is not lit.
  for (let k = 0; k < head.length; ++k) {
    const a = head[k], b = head[(k + 1) % head.length], m = [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2];
    if (!winside(ear[0], m) && !winside(ear[1], m)) wln(4, a, b);
  }
  for (const e of ear) for (let s = 0; s < 4; ++s) {
    // An ear's edges in halves, those inside the head left dark.
    const a = e[s], b = e[s + 1];
    for (let j = 0; j < 2; ++j) {
      const p = [a[0] + (b[0] - a[0]) * j / 2, a[1] + (b[1] - a[1]) * j / 2];
      const r = [a[0] + (b[0] - a[0]) * (j + 1) / 2, a[1] + (b[1] - a[1]) * (j + 1) / 2];
      if (!winside(head, [(p[0] + r[0]) / 2, (p[1] + r[1]) / 2])) wln(4, p, r);
    }
  }
  for (const Q of inner) fill(Q, 3, []);
  // Whiskers catch the back light past the cheeks; they twitch.
  for (let k = 0; k < 6; ++k) {
    const w = WHISK[k % 3], sg = k < 3 ? -1 : 1, x = sg * (w[0] + w[2]) / 2, y = (w[1] + w[3]) / 2;
    wln(4, tf(x, y, 22), tf(sg * w[2], w[3] + 2 * M.sin(t * .3 + k), 14));
  }
  // Eyes (almonds on the face, slanted): wide in the dark, the pupils big;
  // the far eye narrows with the turn by itself. Shut: a line.
  const op = q[6], h = 8 * op;
  for (const sg of [-1, 1]) {
    const E = (x, y) => {
      const p = wsurf(sg * 21 + x, -7 + y - sg * x * .18, 1);
      return tf(p[0], p[1], p[2]);
    };
    if (op < .2) { wln(4, E(-11, 0), E(-4, 2)); wln(4, E(-4, 2), E(4, 2)); wln(4, E(4, 2), E(11, 0)); continue; }
    const px = q[5] * 4.4, pw = t < 40 ? 4.4 : 3, ph = h * .92, I = [], O = [];
    for (let k = 0; k < 12; ++k) {
      const g = k * M.PI / 6, cg = M.cos(g), s1 = M.sin(g);
      I.push(E(11 * cg, h * s1 * (1 - .3 * cg * cg)));
      O.push(E(px + pw * cg, ph * s1));
    }
    fill(I, 1, [O]);
    wln(5, E(px - pw * .8, -h * .45), E(px - pw * .2, -h * .45));
  }
  return {b: wakeB(t), s: 0, d: WD};
}
