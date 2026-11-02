'use strict';
// ---- BIG WAVE's Shiki programs (docs/kasane/shiki-spec.md sec.8) as JS
// functions. The decoder is marked rom: the build (tools/make_app_chunks.py,
// main/CMakeLists.txt) compiles them into the firmware's built-in Shiki table
// (tools/kasane_ir/emit_shiki_rom.mjs) and ships this file without them; the
// app registers 'bigwave.<name>' with the function's arguments (big_wave.js).
// The camera (vanishing point 160, 56; slice step D 1.8 m) and the face
// profile (PF: L -1, V 1.25, l -1.35, v 1.05, dl 1.45, dv -.5) are literals
// here; big_wave.js keeps the same numbers in C and PF. Comments before a
// program and inside it do not ship.
/** @planDecoder rom */
function prog() {
  throw Error('programs are compiled at build time: tools/kasane_ir/lower_plans.mjs');
}
const T = {
  // The slice loop of cu, sh, se and fo: y is 1/depth of the slice; the next
  // slice is D m further, and there is no divide, so y *= 1 - u + u^2 - u^3
  // with u = D y (README). y is small (0.016..0.2): Q24.

  // cu: n slices of the curling face, each a CUBIC from the trough (x0, y0)
  // to the lip (x3, y3), the lips joined by lines. (ex, ey): the wave's
  // height times the roll basis F e; (wx, wy): the camera offset F w; q the
  // curl at the first slice, dq its step.
  /** @shiki cu inputs: ex, ey, wx, wy, y0, n, q0, dq
   *  @q y=24, y0=24, u=24 */
  cu(c, seg) {
    let y = y0, q = q0;
    for (let i = 0; i < n; i++) {
      const ax = y * ex, ay = y * ey;
      const x0 = y * wx + 160, z0 = y * wy + 56;
      const x1 = x0 - ax, z1 = z0 - ay;
      const x2 = x1 + 1.25 * ay, z2 = z1 - 1.25 * ax;
      const k1 = q * 1.45 - 1.35, k2 = q * (-.5) + 1.05;
      const x3 = x0 + k1 * ax + k2 * ay, z3 = z0 + k1 * ay - k2 * ax;
      line(x3, z3, c);
      cubic(seg, c, x0, z0, x1, z1, x2, z2, x3, z3);
      q += dq;
      const u = 1.8 * y;
      y = y * (1 - u * (1 - u * (1 - u)));
    }
  },
  // sh: the shoulder, past the curl: the same slices, the lip fixed (no curl)
  // and the height h scaled per slice (q is h here; ex, ey: F e).
  /** @shiki sh inputs: ex, ey, wx, wy, y0, n, q0, dq
   *  @q y=24, y0=24, u=24 */
  sh(c, seg) {
    let y = y0, q = q0;
    for (let i = 0; i < n; i++) {
      const s = y * q, ax = s * ex, ay = s * ey;
      const x0 = y * wx + 160, z0 = y * wy + 56;
      const x1 = x0 - ax, z1 = z0 - ay;
      const x2 = x1 + 1.25 * ay, z2 = z1 - 1.25 * ax;
      const x3 = x0 - 1.35 * ax + 1.05 * ay, z3 = z0 - 1.35 * ay - 1.05 * ax;
      line(x3, z3, c);
      cubic(seg, c, x0, z0, x1, z1, x2, z2, x3, z3);
      q += dq;
      const u = 1.8 * y;
      y = y * (1 - u * (1 - u * (1 - u)));
    }
  },
  // se: the sea's slices out to the horizon: the near edge in t and the
  // 8 m span in s, a dot of spray in g wandering along it (sin of the depth
  // and the time ph, which big_wave.js keeps within 2 pi).
  /** @shiki se inputs: ex, ey, wx, wy, y0, n, ph
   *  @q y=24, y0=24, u=24 */
  se(t, s, g) {
    let y = y0;
    for (let i = 0; i < n; i++) {
      const ax = y * ex, ay = y * ey;
      const x0 = y * wx + 160, z0 = y * wy + 56;
      const x2 = x0 + 8 * ax, z2 = z0 + 8 * ay;
      line(x0, z0, t);
      move(x2, z2);
      line(x0, z0, s);
      const w = (sin(y * 97 + ph) * .45 + .5) * 8;
      plot(x0 + w * ax, z0 + w * ay, g);
      const u = 1.8 * y;
      y = y * (1 - u * (1 - u * (1 - u)));
    }
  },
  // sk: the sky: the horizon line at depth 1 / h (h = F / 250) in k, then 5
  // rays from the vanishing point out to depths 2, 3.2, 5.12 ... (r *= 1.6) in t.
  /** @shiki sk inputs: ex, ey, wx, wy, y */
  sk(k, t) {
    const ax = y * ex, ay = y * ey;
    const x0 = y * wx + 160, z0 = y * wy + 56;
    move(160, 56);
    line(1.6666667 * ex + 160, 1.6666667 * ey + 56, k);
    let r = 2;
    for (let i = 0; i < 5; i++) {
      move(160, 56);
      line(x0 + r * ax, z0 + r * ay, t);
      r = r * 1.6;
    }
  },
  // fo: whitewater: per slice a ragged line from l down the face in m steps
  // of d, offset across by (sin(1.9 r + ph) + 1) amp; the phase moves 2.3 a
  // slice. l, d in m of the wave (set by the height), m the load tier's.
  /** @shiki fo inputs: ex, ey, wx, wy, y0, n, ph0, amp
   *  @q y=24, y0=24, u=24 */
  fo(l, m, d, c) {
    let y = y0, ph = ph0;
    for (let i = 0; i < n; i++) {
      const ax = y * ex, ay = y * ey;
      const x0 = y * wx + 160, z0 = y * wy + 56;
      move(x0, z0);
      let r = l;
      for (let j = 0; j < m; j++) {
        const a = (sin(1.9 * r + ph) + 1) * amp;
        line(x0 + r * ax + a * ay, z0 + r * ay - a * ax, c);
        r += d;
      }
      ph += 2.3;
      const u = 1.8 * y;
      y = y * (1 - u * (1 - u * (1 - u)));
    }
  },
  // ri: another rider at (px, py), scale (kc, ks) = k (cos, sin) of the
  // roll, size s, in colour c: board, legs and body as lines.
  /** @shiki ri inputs: px, py, kc, ks, s, c */
  ri() {
    const ux = ks * s, uy = -(kc * s), bx = .3 * kc, by = .3 * ks;
    const hx = .85 * ux + px, hy = .85 * uy + py;
    move(px - bx, py - by);
    line(hx, hy, c);
    line(px + bx, py + by, c);
    line(px - bx, py - by, c);
    move(hx, hy);
    line(1.85 * ux + px, 1.85 * uy + py, c);
    const ax = 1.25 * ux + px - 2 * bx, ay = 1.25 * uy + py - 2 * by;
    move(ax, ay);
    line(ax + 4 * bx, ay + 4 * by, c);
  },
  // ro: a rock at (px, py), scale (kc, ks): one CUBIC hump and its base.
  /** @shiki ro inputs: px, py, kc, ks */
  ro() {
    const x0 = px - 1.2 * kc, y0 = py - 1.2 * ks;
    cubic(6, 35953, x0, y0, px - kc + 1.3 * ks, py - ks - 1.3 * kc, .9 * kc + px + 1.5 * ks, .9 * ks + py - 1.5 * kc,
      1.2 * kc + px, 1.2 * ks + py);
    line(x0, y0, 35953);
  },
  // ln: one line (the finish, the shadow).
  /** @shiki ln inputs: x0, y0, x1, y1, c */
  ln() {
    move(x0, y0);
    line(x1, y1, c);
  }
};
