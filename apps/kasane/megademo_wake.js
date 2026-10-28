// MEGADEMO chunk 3: WAKE, the prologue (Tama in the dark). act2's scene
// table calls wake(); the frame loop draws it through emit.
'use strict';
// WAKE: Tama peering in, backlit in the dark, in close-up: the head fills
// the view, so only the eyes, the ears and part of the outline show (never
// the whole simple shape). The backdrop is the silhouette and the lit room
// behind it is the fill (screen minus head and ears: the smaller area, kept
// under the 8192 raster steps a frame), in tz trapezoids of horizontal
// lines. A cyan rim on the outer edges, magenta inner ears (the icon's
// cyber coat), glowing eyes. The head acts Live2D-like on flat shapes: a
// turn (yaw) moves the eyes across and the ears the other way, narrows the
// far eye and ear and compresses the far side; a nod (pitch) moves them up
// and down; roll tilts it; the pupils glance on their own. POSE keys:
// [t, scale, yaw, pitch, roll, gaze]. wakeAt(t) slides it in, then breathes.
// From 42 (after the viewer sits up) the TV is off.
const wakeAt = t => [rnd(8 * (1 - smooth(ramp(t, 4, 18))) + 2 * M.sin(t * .11)), rnd(1.5 * M.sin(t * .2))];
const POSE = [[0, 1.36, .3, 0, -.08, 0], [15, 1.36, .3, 0, -.08, 0], [17, 1.36, .3, 0, -.08, -1],
  [22, 1.38, -.45, .05, -.02, -1], [27, 1.48, .05, -.2, -.1, 0], [31, 1.52, .05, -.25, -.12, 0],
  [37, 1.48, .12, -.12, -.24, .25], [42, 1.48, .12, -.12, -.24, .25]];
// The back light: an elliptical glow behind the head's top left; the rest
// of the room stays as dark as the cat, outlined only by its rim.
const BACK = [128, 36, 84, 38];
// Ear bases sit just inside the head's outline.
const HEAD = [], EARS = [[[-43, -22], [-42, -70], [-10, -40]], [[10, -42], [40, -72], [44, -24]]];
for (let i = 0; i < 24; ++i) {
  const a = i / 24 * 2 * M.PI, s = M.sin(a);
  HEAD.push([52 * M.cos(a) * (1 + .06 * s), 41 * s + (s > 0 ? 6 * s * s : 0)]);
}
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
  if (a[0] > 239 && b[0] > 239 || a[1] < 0 && b[1] < 0 || a[0] < 0 && b[0] < 0) return;
  const v = WEMIT ? WLA : [0, 0, 0, 0];
  v[0] = a[0]; v[1] = a[1]; v[2] = b[0]; v[3] = b[1];
  if (WEMIT) WEMIT(k, v); else WD.push([k, v]);
}
// WAKE's backdrop: the silhouette, until the TV is off.
const wakeB = t => t < 42 ? 0x0043 : 0;
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
const WCORE = [], WRIM = [];
for (let j = 0; j < 16; ++j) WCORE.push([BACK[0] + BACK[2] * .68 * M.cos(j * M.PI / 8), BACK[1] + BACK[3] * .68 * M.sin(j * M.PI / 8)]);
function wake(t, emit) {
  WD = []; WEMIT = emit || null;
  if (t >= 42) return {b: 0, s: 0, d: WD};
  let i = 0;
  while (i < POSE.length - 2 && POSE[i + 1][0] <= t) ++i;
  const A = POSE[i], B = POSE[i + 1], u = smooth(ramp(t, A[0], B[0])), q = A.map((v, j) => v + (B[j] - v) * u);
  const S = q[1], Y = q[2], P = q[3], c = M.cos(q[4]), sn = M.sin(q[4]), o = wakeAt(t);
  // Flat to screen: the far side compresses, a point at depth z (1: the
  // eyes, in front; < 0: the ears, behind) slides with the turn and nod.
  const tf = (x, y, z) => {
    x = x * (x * Y > 0 ? 1 - .2 * M.abs(Y) : 1 + .06 * M.abs(Y)) + 14 * Y * z;
    y += -12 * P * z;
    return [130 + o[0] + (x * c - y * sn) * S, 104 + o[1] + (x * sn + y * c) * S];
  };
  const head = HEAD.map(p => tf(p[0], p[1], 0));
  const ear = EARS.map((e, k) => e.map((p, j) => tf(p[0] + (k && j === 1 && t >= 21 && t < 24 ? 7 : 0),
    p[1] + (k && j === 1 && t >= 21 && t < 24 ? 4 : 0), j === 1 ? -.7 : -.2)));
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
  for (let k = 0; k < 24; ++k) {
    const a = head[k], b = head[(k + 1) % 24], m = [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2];
    if (!winside(ear[0], m) && !winside(ear[1], m)) wln(4, a, b);
  }
  for (const e of ear) {
    // An ear's edges in quarters, those inside the head left dark.
    for (let s = 0; s < 2; ++s) {
      const a = e[s], b = e[s + 1];
      for (let j = 0; j < 4; ++j) {
        const p = [a[0] + (b[0] - a[0]) * j / 4, a[1] + (b[1] - a[1]) * j / 4];
        const r = [a[0] + (b[0] - a[0]) * (j + 1) / 4, a[1] + (b[1] - a[1]) * (j + 1) / 4];
        if (!winside(head, [(p[0] + r[0]) / 2, (p[1] + r[1]) / 2])) wln(4, p, r);
      }
    }
    const m = [(e[0][0] + e[2][0]) / 2, (e[0][1] + e[2][1]) / 2];
    fill([[e[0][0] * .45 + m[0] * .55, e[0][1] * .45 + m[1] * .55], [e[1][0] * .6 + m[0] * .4, e[1][1] * .6 + m[1] * .4],
      [e[2][0] * .45 + m[0] * .55, e[2][1] * .45 + m[1] * .55]], 3, []);
  }
  // Eyes: open (watching, pupils wide in the dark), a slow blink at 30,
  // then at ease. The far eye narrows with the turn.
  const op = t < 30 ? 1 : t < 34 ? M.abs(t - 32) / 2 : .55;
  for (const ex of [-22, 22]) {
    const w = 10 * (ex * Y > 0 ? 1 - .45 * M.abs(Y) : 1), ey = ex < 0 ? -6 : -8, h = 7 * op;
    const E = (x, y) => tf(ex + x, ey + y, 1);
    if (op < .2) { wln(4, E(-w, 0), E(0, 2)); wln(4, E(0, 2), E(w, 0)); continue; }
    const px = q[5] * w * .45, pw = w * (t < 34 ? .38 : .28), ph = h * .88;
    fill([E(-w, 0), E(-w * .4, -h), E(w * .4, -h), E(w, 0), E(w * .4, h), E(-w * .4, h)], 1,
      [[E(px - pw, -ph), E(px + pw, -ph), E(px + pw * 1.25, 0), E(px + pw, ph), E(px - pw, ph), E(px - pw * 1.25, 0)]]);
    wln(5, E(px - pw * .7, -h * .4), E(px - pw * .2, -h * .4));
  }
  return {b: wakeB(t), s: 0, d: WD};
}
