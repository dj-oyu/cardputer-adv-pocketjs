'use strict';
// ---- STRESS TEST's Shiki programs (docs/kasane/shiki-spec.md sec.8). The
// decoder is marked rom: the build (tools/make_app_chunks.py,
// main/CMakeLists.txt) compiles them into the firmware's built-in Shiki
// table as stress.<name> and ships this file without them; stress.js
// registers 'stress.<name>'. The horizon (y 60) and the sun's centre (x 176)
// are literals here and in stress.js. Comments before a program and inside
// it do not ship.
/** @planDecoder rom */
function prog() {
  throw Error('programs are compiled at build time: tools/kasane_ir/lower_plans.mjs');
}
const T = {
  // sky: n full-width rows from y0, each in its row's colour of the ramp
  // (135 colours, one per row: sky, horizon, sea; stress.js darkens every
  // other row, the scanlines). 34 rows is the most a draw may raster
  // (34 x 240 <= 8,192), so the screen is four draws.
  /** @shiki sky inputs: y0, n
   *  @data ramp: u16 q0 record 1 */
  sky() {
    let y = y0;
    for (let i = 0; i < n; i++) {
      move(0, y);
      line(239, y, ramp[y]);
      y += 1;
    }
  },
  // stars: per record a point at (x, y) twinkling with sin(ph + t): the
  // brightness picks one of the 8 colours of tw.
  /** @shiki stars inputs: t
   *  @data at: i16 q0 record 3
   *  @data tw: u16 q0 record 1 */
  stars() {
    for (const [x, y, ph] of at) {
      const b = sin(ph * .1 + t);
      plot(x, y, tw[b * 3.5 + 3.5]);
    }
  },
  // sun: one row per record, a chord of the disc centred on x 176. stress.js
  // moves the rows of the gaps off the screen (y 300) each frame, so the
  // stripes climb.
  /** @shiki sun
   *  @data rows: u16 q0 record 3 */
  sun() {
    for (const [y, w, c] of rows) {
      move(176 - w, y);
      line(176 + w, y, c);
    }
  },
  // glow: the sun's path on the water, 14 dashes below the horizon, shorter
  // with depth, each breathing with its own phase.
  /** @shiki glow inputs: t
   *  @q d=0 */
  glow(c, d) {
    let y = 62, h = 16, ph = t, col = c;
    for (let k = 0; k < 14; k++) {
      const w = h * (sin(ph) * .35 + .65);
      move(176 - w, y);
      line(176 + w, y, col);
      col = d - col;
      y += 3.2;
      h -= .9;
      ph += 1.7;
    }
  },
  // waves: 12 swell lines from the horizon to the front, spaced as depth
  // (62 + r^2 / 1.6), each a polyline of 20 steps whose swell grows nearer.
  // The colour of line r is wc[r].
  /** @shiki waves inputs: t
   *  @data wc: u16 q0 record 1 */
  waves() {
    let base = 62, gap = .6, amp = .4, ph = t, f = .045;
    for (let r = 0; r < 12; r++) {
      let x = -6, a = ph;
      move(x, base + amp * sin(a));
      for (let i = 0; i < 20; i++) {
        x += 13;
        a += f * 13;
        line(x, base + amp * sin(a), wc[r]);
      }
      base += gap;
      gap += 1.25;
      amp += .38;
      ph += .9;
      f -= .0025;
    }
  },
  // trunk / crown: a palm's silhouette standing at (x, y), leaning to m (1
  // right, -1 left), its top swayed by s. The trunk is 3 curves; the crown
  // a frond per record of fr (control point (cx, cy) and tip (ex, ey) from
  // the top, x mirrored by m), 3 curves meeting at the tip for a leaf's width.
  /** @shiki trunk inputs: x, y, m, s */
  trunk(c, seg) {
    const tx = x + 16 * m + s, ty = y - 64;
    let o = -1;
    for (let k = 0; k < 3; k++) {
      cubic(seg, c, x + o, y, x + o + 3 * m, y - 24, tx + o - 6 * m, ty + 22, tx + o, ty);
      o += 1;
    }
  },
  /** @shiki crown inputs: x, y, m, s
   *  @data fr: i16 q0 record 4 */
  crown(c, seg) {
    const tx = x + 16 * m + s, ty = y - 64;
    for (const [cx, cy, ex, ey] of fr) {
      const px = tx + ex * m + s, py = ty + ey + s * .3, qx = tx + cx * m, qy = ty + cy;
      cubic(seg, c, tx, ty, qx, qy, px - 3 * m, py - 5, px, py);
      cubic(seg, c, tx, ty + 1, qx, qy + 3, px - 3 * m, py - 2, px, py);
      cubic(seg, c, tx, ty + 2, qx, qy + 5, px - 4 * m, py, px, py);
    }
  },
  // fish: a school at (x, y): per record a fish at (dx, dy), a body (a
  // wedge, nose to the right) and a tail, in its colour.
  /** @shiki fish inputs: x, y
   *  @data school: u16 q0 record 3 */
  fish() {
    for (const [dx, dy, c] of school) {
      const fx = x + dx, fy = y + dy;
      move(fx + 3, fy);
      line(fx + 12, fy + 2, c);
      line(fx + 3, fy + 4, c);
      line(fx + 3, fy, c);
      move(fx, fy - 1);
      line(fx + 3, fy + 2, c);
      line(fx, fy + 5, c);
    }
  },
  // dolphin: the outline polyline at (x, y), and the belly line.
  /** @shiki dolphin inputs: x, y
   *  @data shape: i16 q0 record 2 */
  dolphin(c, b) {
    for (const [px, py] of shape.take(1)) { move(x + px, y + py); }
    for (const [px, py] of shape.from(1)) { line(x + px, y + py, c); }
    move(x + 9, y + 10);
    line(x + 30, y + 10, b);
  },
  // bubbles: per record a ring of radius r at (x, y), 8 sides.
  /** @shiki bubbles
   *  @data ring: i16 q0 record 3 */
  bubbles(c) {
    for (const [bx, by, r] of ring) {
      let a = 0;
      move(bx + r, by);
      for (let k = 0; k < 8; k++) {
        a += .7853982;
        line(bx + r * sin(a + 1.5707963), by + r * sin(a), c);
      }
    }
  },
  // vhs: a tracking glitch, two ragged lines at y (stress.js parks it off
  // the screen between glitches): 16 steps across, each jittered by sin.
  /** @shiki vhs inputs: y, t */
  vhs(c, d) {
    let x = 0, a = t;
    move(0, y);
    for (let i = 0; i < 16; i++) {
      x += 16;
      a += 2.9;
      line(x, y + sin(a) * 1.5, c);
    }
    move(0, y + 3);
    line(239, y + 3, d);
  },
};
