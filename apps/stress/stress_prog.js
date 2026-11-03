'use strict';
// ---- STRESS TEST's Shiki programs (docs/kasane/shiki-spec.md sec.8). The
// decoder is marked rom: the build (tools/make_app_chunks.py,
// main/CMakeLists.txt) compiles them into the firmware's built-in Shiki
// table as stress.<name> and ships this file without them; stress.js
// registers 'stress.<name>'. The vanishing point (120, 61), the horizon
// (y 60) and the sun's centre (x 176) are literals here and in stress.js.
// Comments before a program and inside it do not ship.
/** @planDecoder rom */
function prog() {
  throw Error('programs are compiled at build time: tools/kasane_ir/lower_plans.mjs');
}
const T = {
  // sky: n full-width rows from y0, each in its row's colour of the ramp
  // (135 colours, one per row: sky, horizon, sea). 34 rows is the most a
  // draw may raster (34 x 240 <= 8,192), so the screen is four draws.
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
  // sun: one row per record, a chord of the disc centred on x 176.
  /** @shiki sun
   *  @data rows: u16 q0 record 3 */
  sun() {
    for (const [y, w, c] of rows) {
      move(176 - w, y);
      line(176 + w, y, c);
    }
  },
  // grid: 5 horizontal lines below the horizon at depth q = p + 9.6 k
  // (y = 62 + q^2 / 50: faster near us; stress.js keeps p in 0..9.6), and 9
  // rays from the vanishing point.
  /** @shiki grid inputs: p */
  grid(c) {
    let q = p;
    for (let k = 0; k < 5; k++) {
      const y = 62 + q * q * .02;
      move(0, y);
      line(239, y, c);
      q += 9.6;
    }
    let x = -120;
    for (let j = 0; j < 9; j++) {
      move(120, 61);
      line(x, 134, c);
      x += 60;
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
};
