// DERBY WATCH's crowd as it was before the pattern line (vm/main 45f309f,
// apps/derby/derby_prog.js): the plans crowd_look.mjs and crowd_p24.mjs draw
// "today" with (the checkerboard dots) and the background they share. Host
// only, frozen: the app now draws the crowd with the pattern line.
const T = {
  // stands: $0 tier lines across the screen from the ground up (rise < 0 per
  // tier), then per bay a pillar from the ground to the roof and a roof arc
  // of $3 segments peaking two tiers above the roof line. The x + 0 copies
  // are the hand IR's (x + 0 is not x: it turns -0 into +0), kept so the IR
  // did not change in the move to JS.
  /** @plan stands inputs: x0, dx, ground, rise, bays, unused5, unused6, roof */
  stands(tiers, rows, dots, arcs) {
    let x = x0;
    let y = ground + 0;
    for (let k = 0; k < tiers; k++) {
      move(0, y);
      line(239, y, 21130);
      y += rise;
    }
    const peak = roof + rise + rise;
    const peak2 = peak + 0;
    for (let k = 0; k < bays; k++) {
      const a = x + 0;
      const b = x + 0;
      const c = x + dx;
      const d = x + dx;
      move(x, ground);
      line(x, roof, 31727);
      cubic(arcs, 50712, a, roof, b, peak, c, peak2, d, roof);
      x += dx;
    }
  },

  // crowd: the stand's spectators as dots. $1 rows of in(4) bays of $2 dots,
  // each dot swaying on its own phase; colours cycle through two values
  // (105642 minus the last) seeded by input 6.
  // q27 prototype (docs/kasane/derby-background-cost.md): half the dots per
  // frame, every other index from `off` (input 7, the frame's parity); the
  // next frame draws the other half. Each row starts at the other parity (a
  // checkerboard). A frame's dots in a row are one colour, dc: input 6 is
  // today's seed xor the parity, and dc flips once a row (the start moved by
  // one) and once a bay when a bay has an odd count ($7 + $7 - $2). The
  // loop runs ceil(dots / 2) ($7) a bay and stops past the row's last index.
  // (Packed plans carry 8 arguments, $0..$7.)
  /** @plan crowd inputs: x0, spread, y0, rowGap, bays, sway, seed, parity */
  crowd(p0, p1, p2, p3, p4, p5, p6, p7) {
    let dc = seed * 12650 + 46496;
    const step = p6 * spread;
    const step2 = step + step;
    const lim = bays * spread + x0 - step * .5;
    let off = parity;
    let ps = sway;
    let y = rowGap * .5 + y0;
    for (let j0 = 0; j0 < p1; j0++) {
      let x = off * step + x0;
      let phase = off * 2.39996 + ps;
      for (let j1 = 0; j1 < bays * p7; j1++) {
        if (x > lim) break;
        plot(sin(phase) * 1.5 + x, y, dc);
        x += step2;
        phase += 4.79992;
      }
      for (let j2 = 0; j2 < (p7 + p7 - p2) * bays; j2++) {
        dc = 105642 - dc;
      }
      dc = 105642 - dc;
      y += rowGap;
      ps += .9;
      off = 1 - off;
    }
  },

  // ---- The panning units (ser() in scene, README "首振りカメラ"). A series
  // of points along the straight, far to near: X'' and -Z' step linearly,
  // and 1/Z' comes from the point before by Newton, q *= (-Z') q + 2. The
  // start is farther, so 1/Z' rises to the true value and cannot diverge.
  // Screen x = X'' q, the height e at y = 28 + (6 - e) q (the units' height
  // 6 m and horizon 28 are baked in).
  // prail: posts from the height 6 - top down to the ground, and the top and
  // mid lines from the first post to the point after the last.
  /** @plan prail inputs: x, dx, z, dz, posts, r, colour, top */
  prail() {
    let px = x, pz = z, q = r;
    const x0 = px * q;
    for (let j = 0; j < posts; j++) {
      const sx = px * q;
      move(sx, q * top + 28);
      line(sx, q * 6 + 28, colour);
      px += dx;
      pz += dz;
      const a = q * (pz * q + 2);
      const b = a * (pz * a + 2);
      q = b * (pz * b + 2);
    }
    const x1 = px * q;
    move(x0, r * top + 28);
    line(x1, q * top + 28, colour);
    const mid = (top + 6) * .5;
    move(x0, r * mid + 28);
    line(x1, q * mid + 28, colour);
  },

  // pk: the crowd as the side view draws it (crowd), a column of $1 rows
  // every 12/$2 m, row i at 1.2 + 2.4 i m (hh = 6 - that), half the dots a
  // frame: rows of one parity in a column, the other in the next (the first
  // column starts on row 0). Each dot swayed 1.5 px by sin of its phase, the
  // column's + .9 i (phase is the first column's + 1.8; +2.39996 a column).
  // The colour flips a column, as crowd's does when a row has an even count
  // (with an odd count crowd's depends on the bays in view). Rows past the
  // top stop at lo.
  /** @plan pk inputs: x, dx, z, dz, cols, r, phase, colour */
  pk(p0, rows) {
    let dc = colour;
    let px = x, pz = z, q = r, h = 4.8, a = phase;
    const lo = rows * -2.4 + 6.2;
    for (let j = 0; j < cols; j++) {
      const sx = px * q;
      let hh = h;
      for (let k = 0; k < rows; k++) {
        if (lo > hh) break;
        plot(sin(hh * -.375 + a) * 1.5 + sx, hh * q + 28, dc);
        hh += -4.8;
      }
      h = 7.2 - h;
      dc = 105642 - dc;
      a += 2.39996;
      px += dx;
      pz += dz;
      const a1 = q * (pz * q + 2);
      const a2 = a1 * (pz * a1 + 2);
      q = a2 * (pz * a2 + 2);
    }
  },

  // hl: lines between two ends (x0, 1/Z' r0) and (x1, r1): the first at the
  // height coefficient c0, then cs a line, lines of them; the ends of the
  // first and the last joined (the screen's face and bezel, stand tiers).
  /** @plan hl inputs: x0, r0, x1, r1, c0, cs, lines, colour */
  hl() {
    let c = c0, ya = 0, yb = 0;
    const y0 = r0 * c0 + 28;
    const y1 = r1 * c0 + 28;
    for (let j = 0; j < lines; j++) {
      ya = r0 * c + 28;
      move(x0, ya);
      yb = r1 * c + 28;
      line(x1, yb, colour);
      c += cs;
    }
    move(x0, y0);
    line(x0, ya, colour);
    move(x1, y1);
    line(x1, yb, colour);
  },

};
