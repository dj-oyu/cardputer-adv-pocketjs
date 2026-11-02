'use strict';
// ---- MEGADEMO's Shiki programs (docs/kasane/shiki-spec.md sec.8) as JS
// functions, one per template of the old text form (megademo_plans.js until
// v2.0). The decoder is marked rom: the build (tools/make_app_chunks.py,
// main/CMakeLists.txt) compiles them into the firmware's built-in Shiki
// table as kasane.<name> and ships this file without them; megademo_set.js
// registers 'kasane.<name>' with an entry's arguments (plist(),
// megademo_act2.js). The screen centre 120, 67 is a literal. Comments
// before a program and inside it do not ship.
/** @planDecoder rom */
function prog() {
  throw Error('programs are compiled at build time: tools/kasane_ir/lower_plans.mjs');
}
const T = {
  // ---- Act I's HUD (megademo_brief.js).
  // cw: the carrier, 49 points 5 px apart: sin(0.55 i + ph) times a
  // modulation 1 + depth sin(0.13 i + mph), amplitude amp about y0.
  /** @shiki cw inputs: y0, amp, ph, depth, mph */
  cw(c) {
    let x = 0, i = 0;
    for (let k = 0; k < 49; k++) {
      const m = sin(i * .55 + ph) * (sin(i * .13 + mph) * depth + 1);
      line(x, m * amp + y0, c);
      x += 5;
      i += 1;
    }
  },
  // ch / cv: n ticks of length L from the line at, p0 every sp, across or down.
  /** @shiki ch inputs: at, L, p0, sp, n */
  ch(c) {
    const e = at + L;
    let p = p0;
    for (let k = 0; k < n; k++) { move(p, at); line(p, e, c); p += sp; }
  },
  /** @shiki cv inputs: at, L, p0, sp, n */
  cv(c) {
    const e = at + L;
    let p = p0;
    for (let k = 0; k < n; k++) { move(at, p); line(e, p, c); p += sp; }
  },
  // eg: a brain wave from x 6, 2.4 px a sample: sin(s rh) + sin(s^2 noise) +
  // 2 sin(s pi/32)^16 (a spike every 32 samples), amplitude amp about y0.
  // s^2 noise passes Q16, so its phase p steps by d = (2 s + 1) noise, d by
  // 2 noise; the app passes both at s0, reduced mod 2 pi (megademo_brief.js).
  /** @shiki eg inputs: y0, amp, s0, rh, noise, col, p0, d0 */
  eg() {
    let x = 6, s = s0, p = p0, d = d0;
    for (let k = 0; k < 48; k++) {
      const q = sin(s * .0981748), q2 = q * q, q4 = q2 * q2, q8 = q4 * q4, q16 = q8 * q8;
      const v = sin(s * rh) + sin(p) + q16 + q16;
      line(x, v * amp + y0, col);
      x += 2.4;
      s += 1;
      p += d;
      d += noise + noise;
    }
  },
  // sp: a spectrum's 15 bars on the base line 131 from x 128: bar k is
  // sin(7.31 k + ph)^2 (28.6 - 1.3 k) gain + 2 high, 3 px wide.
  /** @shiki sp inputs: ph, gain */
  sp(c) {
    let x = 128, h = 28.6, k = 0;
    for (let i = 0; i < 15; i++) {
      const s = sin(k + ph), top = 131 - (s * s * h * gain + 2);
      line(x, 131, c);
      line(x, top, c);
      x += 3;
      line(x, top, c);
      line(x, 131, c);
      x += 1;
      k += 7.31;
      h -= 1.3;
    }
  },
  // sc: a Lissajous 3:2 about 214, 119.
  /** @shiki sc inputs: ph */
  sc(c) {
    let a = 0, b = 0;
    for (let i = 0; i < 41; i++) {
      line(sin(a + ph) * 18 + 214, sin(b) * 11 + 119, c);
      a += .4712;
      b += .3142;
    }
  },
  // ---- TWIST (megademo_act2.js). Points about the screen centre 120, 67,
  // turned each step by (co, si): x' = x co - y si, y' = y co + x si.
  // fr: n frames, the quad (x0, y0) (x1, y1) (x2, y2) (x0 + x2 - x1, ...).
  /** @shiki fr inputs: x0, y0, x1, y1, x2, y2, co, si */
  fr(n, c) {
    let ax = x0, ay = y0, bx = x1, by = y1, cx = x2, cy = y2, dx = x0 + x2 - x1, dy = y0 + y2 - y1;
    for (let i = 0; i < n; i++) {
      move(ax + 120, ay + 67);
      line(bx + 120, by + 67, c);
      line(cx + 120, cy + 67, c);
      line(dx + 120, dy + 67, c);
      line(ax + 120, ay + 67, c);
      const ax1 = ax * co - ay * si; ay = ay * co + ax * si; ax = ax1;
      const bx1 = bx * co - by * si; by = by * co + bx * si; bx = bx1;
      const cx1 = cx * co - cy * si; cy = cy * co + cx * si; cx = cx1;
      const dx1 = dx * co - dy * si; dy = dy * co + dx * si; dx = dx1;
    }
  },
  // ra: two polylines, each point turned before the next line.
  /** @shiki ra inputs: x0, y0, x1, y1, co, si */
  ra(n, c) {
    let ax = x0, ay = y0, bx = x1, by = y1;
    for (let i = 0; i < n; i++) {
      move(ax + 120, ay + 67);
      const ax1 = ax * co - ay * si; ay = ay * co + ax * si; ax = ax1;
      line(ax + 120, ay + 67, c);
      move(bx + 120, by + 67);
      const bx1 = bx * co - by * si; by = by * co + bx * si; bx = bx1;
      line(bx + 120, by + 67, c);
    }
  },
  // ru: n rungs between the two, the colour alternating c1, c2 - c1.
  /** @shiki ru inputs: x0, y0, x1, y1, co, si
   *  @q c2=0 */
  ru(n, c1, c2) {
    let ax = x0, ay = y0, bx = x1, by = y1, col = c1;
    for (let i = 0; i < n; i++) {
      move(ax + 120, ay + 67);
      line(bx + 120, by + 67, col);
      col = c2 - col;
      const ax1 = ax * co - ay * si; ay = ay * co + ax * si; ax = ax1;
      const bx1 = bx * co - by * si; by = by * co + bx * si; bx = bx1;
    }
  },
  // ar: n CUBIC arches from (x0, y0) to (x1, y1), both handles (hx, hy).
  /** @shiki ar inputs: x0, y0, x1, y1, hx, hy, co, si */
  ar(n, seg, c) {
    let ax = x0, ay = y0, bx = x1, by = y1, ux = hx, uy = hy;
    for (let i = 0; i < n; i++) {
      const px = ax + 120, py = ay + 67, qx = bx + 120, qy = by + 67;
      cubic(seg, c, px, py, px + ux, py + uy, qx + ux, qy + uy, qx, qy);
      const ax1 = ax * co - ay * si; ay = ay * co + ax * si; ax = ax1;
      const bx1 = bx * co - by * si; by = by * co + bx * si; bx = bx1;
      const ux1 = ux * co - uy * si; uy = uy * co + ux * si; ux = ux1;
    }
  },
  // ---- ZENITH.
  // st: n stars on an ellipse (a, b) about (x0, y0), pulled by (c, d) times
  // 0.6 sin(1.618 k) - 0.4.
  /** @shiki st inputs: a, b, c, d, ph, x0, y0 */
  st(n) {
    let k = 0;
    for (let i = 0; i < n; i++) {
      k += 1;
      const s = sin(k + ph), t = sin(k * 1.618034) * .6 - .4;
      plot(s * a + x0 + t * c, s * b + y0 + t * d, 65535);
    }
  },
  // sa: n octahedra from (x, y) every (dx, dy): axes (ax, ay) and
  // (-ay, ax) squashed by s, height h; one path over 12 edges.
  /** @shiki sa inputs: x, y, dx, dy, ax, ay, s, h */
  sa(n) {
    const top = h * 1.3, ux = ax, uy = s * ay, vx = 0 - ay, vy = s * ax;
    let px = x, py = y;
    for (let i = 0; i < n; i++) {
      move(px, py - top);
      line(px + ux, py + uy, 65535);
      line(px, py + top, 64063);
      line(px + vx, py + vy, 65535);
      line(px, py - top, 64063);
      line(px - ux, py - uy, 65535);
      line(px, py + top, 64063);
      line(px - vx, py - vy, 65535);
      line(px + ux, py + uy, 65535);
      line(px + vx, py + vy, 65535);
      line(px - ux, py - uy, 65535);
      line(px - vx, py - vy, 65535);
      line(px, py - top, 64063);
      px += dx;
      py += dy;
    }
  },
  // gr: n ground lines (dx, dy) k from (x0, y0), half-width (wx, wy), k
  // growing 1.32 a line, colours alternating 6055 and 64327.
  /** @shiki gr inputs: x0, y0, dx, dy, wx, wy, k0, n */
  gr() {
    let k = k0, col = 6055;
    for (let i = 0; i < n; i++) {
      const a = dx * k + x0, b = dy * k + y0;
      move(a + wx, b + wy);
      line(a - wx, b - wy, col);
      col = 70382 - col;
      k = k * 1.32;
    }
  },
  // la: n lanes, each from 1.5 to f of (u k + (a, b)) about (x0, y0).
  /** @shiki la inputs: x0, y0, a, b, ux, uy, k0, f */
  la(n) {
    let k = k0;
    for (let i = 0; i < n; i++) {
      const px = ux * k + a, py = uy * k + b;
      move(px * 1.5 + x0, py * 1.5 + y0);
      line(px * f + x0, py * f + y0, 5759);
      k += .22;
    }
  },
  // ci: n skyline rows (k growing 1.6 a row), m roof steps of step each,
  // the roof height sin(t k)^2 amp k along (a, b).
  /** @shiki ci inputs: x0, y0, a, b, ux, uy, k0, amp */
  ci(n, m, step) {
    let k = k0;
    for (let i = 0; i < n; i++) {
      let t = -1.6;
      move((t * ux + a) * k + x0, (t * uy + b) * k + y0);
      for (let j = 0; j < m; j++) {
        const s = sin(t * k), r = 0 - s * s * amp * k;
        line((t * ux + a) * k + x0 + r * a, (t * uy + b) * k + y0 + r * b, 64063);
        t += step;
        line((t * ux + a) * k + x0 + r * a, (t * uy + b) * k + y0 + r * b, 64063);
      }
      k = k * 1.6;
    }
  },
  // tr: n CUBIC trails from (x0, y0) to (a, b), handles w and h down.
  /** @shiki tr inputs: x0, y0, a0, b0, da, db, h, w */
  tr(n, seg) {
    let a = a0, b = b0;
    for (let i = 0; i < n; i++) {
      cubic(seg, 64327, x0, y0, x0, y0 + w, a, b + h, a, b);
      a += da;
      b += db;
    }
  },
  // sh: the ship's flame, then its hull: the points of one bank angle
  // (megademo_set.js, in px), a polyline in white.
  /** @shiki sh inputs: x0, y0, x1, y1
   *  @data pts: i16 q0 record 2 */
  sh() {
    move(x0, y0);
    line(x1, y1, 64327);
    for (const [x, y] of pts.take(1)) { move(x, y); }
    for (const [x, y] of pts.from(1)) { line(x, y, 65535); }
  },
  // ln: one line.
  /** @shiki ln inputs: x0, y0, x1, y1 */
  ln(c) {
    move(x0, y0);
    line(x1, y1, c);
  },
  // ---- CORE.
  // tri: a triangle in the colour col.
  /** @shiki tri inputs: x0, y0, x1, y1, x2, y2, col */
  tri() {
    move(x0, y0);
    line(x1, y1, col);
    line(x2, y2, col);
    line(x0, y0, col);
  },
  // qd: a closed quad; sg: two segments.
  /** @shiki qd inputs: x0, y0, x1, y1, x2, y2, x3, y3 */
  qd(c) {
    move(x0, y0);
    line(x1, y1, c);
    line(x2, y2, c);
    line(x3, y3, c);
    line(x0, y0, c);
  },
  /** @shiki sg inputs: x0, y0, x1, y1, x2, y2, x3, y3 */
  sg(c) {
    move(x0, y0);
    line(x1, y1, c);
    move(x2, y2);
    line(x3, y3, c);
  },
  // ht: dot tone in a triangle: n rows from the apex (x0, y0) down one edge
  // (dx, dy), row i i k dots (sx, sy) apart.
  /** @shiki ht inputs: x0, y0, dx, dy, sx, sy, n, k
   *  @q k=0 */
  ht(c) {
    let x = x0, y = y0, m = 0;
    for (let i = 0; i < n; i++) {
      x += dx;
      y += dy;
      m += k;
      let px = x, py = y;
      for (let j = 0; j < m; j++) { plot(px, py, c); px += sx; py += sy; }
    }
  },
  // ry: n rays round (cx, cy) from radius r0 to r1, the angle from a by da.
  /** @shiki ry inputs: cx, cy, r0, r1, a0, n, da, col */
  ry() {
    let a = a0;
    for (let i = 0; i < n; i++) {
      const co = sin(a + 1.5707963), si = sin(a);
      move(co * r0 + cx, si * r0 + cy);
      line(co * r1 + cx, si * r1 + cy, col);
      a += da;
    }
  },
  // nz: scan noise: up to lim of n dashes w long, scattered by sines of t.
  /** @shiki nz inputs: t, lim, w */
  nz(n) {
    let k = 0;
    for (let i = 0; i < n; i++) {
      k += 1;
      if (k > lim) break;
      const y = sin(k * 7.31 + t) * 67 + 67, x = sin(k * 3.97 + t) * 120 + 120;
      move(x, y);
      line(x + w, y, 33808);
    }
  }
};
