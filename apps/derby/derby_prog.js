'use strict';
// ---- DERBY's procedural plans, as JS functions (docs/kasane/js-to-ir.md
// section 5). The decoder is marked rom: they are the firmware's built-in
// plans (docs/kasane/flash-plan.md). The build (tools/make_app_chunks.py,
// main/CMakeLists.txt) compiles them into a const C table
// (tools/kasane_ir/emit_rom_plans.mjs) and ships this file without them
// (lower_plans.mjs --rom): the app registers 'derby.<name>' with the
// function's arguments (load(), derby_view.js). The file does not run
// unlowered (prog() throws). Comments before a plan and inside it do not
// ship; every other comment here does, so keep those short.
/** @planDecoder rom */
function prog() {
  throw Error('plans are compiled at build time: tools/kasane_ir/lower_plans.mjs');
}
// [LIGHT, MID, HEAVY]: stand tiers, crowd rows, dots per bay, roof arc
// segments, rail post and turf stripe spacing (m), the crowd's head and body
// patterns; then the crowd's skin, cloths, row shift, empty, cells a m.
const KN = [[3, 2, 3, 3, 8, 10, 263170, 921095], [4, 3, 4, 5, 5, 7, 1083458, 3792103],
  [5, 4, 6, 8, 4, 5, 2236962, 7829367], [0xf5d3, 0xc228, 0x3a7a, 7, -1, 2.5]];
let tier = 1;
const T = {
  // rail: the running rail. Posts from the top rail to the ground, a top and
  // a mid rail from the first post to one post past the last.
  /** @plan rail inputs: x0, dx, top, ground, posts, mid, colour */
  rail() {
    let x = x0;
    const end = posts * dx + x;
    move(x, top); line(end, top, colour);
    move(x, mid); line(end, mid, colour);
    for (let j0 = 0; j0 < posts; j0++) {
      move(x, top); line(x, ground, colour);
      x += dx;
    }
  },

  // turf: mowing stripes from the near edge (x0, y0) to the far edge (x1, y1),
  // alternating between two greens (512 + 288 * phase, then 1312 minus it).
  /** @plan turf inputs: x0, x1, dx0, dx1, y0, y1, stripes, phase */
  turf() {
    let near = x0, far = x1, green = 512 + phase * 288;
    for (let j0 = 0; j0 < stripes; j0++) {
      move(near, y0); line(far, y1, green);
      near += dx0; far += dx1;
      green = 1312 - green;
    }
  },

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

  // crowd: the stand's spectators (P24, docs/kasane/crowd-primitives-design.md),
  // one draw for the side view and for a stretch of the panning view: $0
  // rows from (x0, y0) to (x1, y1), each row dy0, dy1 above the last at the
  // two ends (-2.4 m / Z', so they are also the ends' depth weights: the VM
  // lays the pattern in perspective). A row is three pattern lines of a
  // 24-cell pattern: the body ($2, 2 px: on the row's line and 1 px above) in
  // $4 or $5 (every other row), the heads ($1) 2 px above in $3. u0, u1 are
  // the pattern's cells at the two ends; a row's pattern is moved $6 cells
  // from the last. $7 is colour B, the empty cells' (-1: none). The loop draws
  // two rows a turn (the two cloths are immediates) and stops after $0.
  // The look's numbers are one table: KN (the tiers' patterns, KN[3] the rest).
  /** @plan crowd inputs: x0, y0, x1, y1, dy0, dy1, u0, u1 */
  crowd(rows, head, body, skin, cloth0, cloth1, shift, empty) {
    let ya = y0, yb = y1, ua = u0, ub = u1, k = rows;
    const s0 = dy0 + 2, s1 = dy1 + 2;
    for (let j = 0; j < rows; j++) {
      move(x0, ya); linePattern(x1, yb, body, cloth0, empty, ua, ub, dy0, dy1, 24);
      ya += -1; yb += -1;
      move(x0, ya); linePattern(x1, yb, body, cloth0, empty, ua, ub, dy0, dy1, 24);
      ya += -1; yb += -1;
      move(x0, ya); linePattern(x1, yb, head, skin, empty, ua, ub, dy0, dy1, 24);
      ya += s0; yb += s1; ua += shift; ub += shift; k += -1;
      if (.5 > k) break;
      move(x0, ya); linePattern(x1, yb, body, cloth1, empty, ua, ub, dy0, dy1, 24);
      ya += -1; yb += -1;
      move(x0, ya); linePattern(x1, yb, body, cloth1, empty, ua, ub, dy0, dy1, 24);
      ya += -1; yb += -1;
      move(x0, ya); linePattern(x1, yb, head, skin, empty, ua, ub, dy0, dy1, 24);
      ya += s0; yb += s1; ua += shift; ub += shift; k += -1;
      if (.5 > k) break;
    }
  },

  // runner: one horse on the gate / FIELD shots. Hip at (x, y), size s; the
  // body in coat $0, four legs to (leg0..3, hoof), the jockey in silk $1 and
  // a cap in $2 (silk ^ 0x8410).
  /** @plan runner inputs: x, y, s, hoof, leg0, leg1, leg2, leg3 */
  runner(p0, p1, p2) {
    const u = s * 2.5, up = -1 * u;
    const back = x + u + u, withers = back + u, head = withers + u;
    const high = y + up, higher = high + up;
    move(x + up, y + u);
    line(x, y, p0); line(withers, y, p0); line(head, higher, p0); line(head + u, high, p0); line(head, high, p0);
    move(x, y); line(leg0, hoof, p0);
    move(x, y); line(leg1, hoof, p0);
    move(withers, y); line(leg2, hoof, p0);
    move(withers, y); line(leg3, hoof, p0);
    move(back, y); line(back, high, p1); line(withers, higher, p1); line(head, high, p1);
    plot(withers, higher, p2);
  },

  // gate: the starting stalls, 9 dividers from the centre outwards, each a
  // shrink times the one before (x, and both heights above the horizon).
  /** @plan gate inputs: x, rise0, rise1, shrink, horizon */
  gate() {
    let dx = x;
    let h0 = rise0;
    let h1 = rise1;
    for (let k = 0; k < 9; k++) {
      const sx = 120 + dx;
      const low = horizon + h1;
      line(sx, low, 40147);
      line(sx, horizon + h0, 40147);
      move(sx, low);
      dx *= shrink;
      h0 *= shrink;
      h1 *= shrink;
    }
  },

  // pole: the winning post (red, from the ground to the top), its disc of
  // radius r as two CUBICs, and the finish line doubled one pixel apart.
  /** @plan pole inputs: x, ground, top, r, lx0, ly0, lx1, ly1 */
  pole() {
    move(x, ground);
    line(x, top, 63488);
    const nr = r * (-1);
    const cy = top + nr;
    const bulge = (-1.33) * r;
    const left = x + nr;
    const c1 = cy + 0;
    const c2 = left + 0;
    const c3 = cy + bulge;
    const right = x + r;
    const c5 = c3 + 0;
    const c6 = right + 0;
    const c7 = cy + 0;
    cubic(6, 65535, left, c1, c2, c3, right, c5, c6, c7);
    const back = bulge * (-1);
    const d3 = cy + back;
    const d5 = cy + back;
    cubic(6, 65535, left, c1, c2, d3, right, d5, c6, c7);
    move(lx0, ly0);
    line(lx1, ly1, 65535);
    move(lx0 + 1, ly0);
    line(lx1 + 1, ly1, 65535);
  },

  // photo: the photo-finish frame. The line at x (white, then red one pixel
  // right), the top and bottom borders, and a ruler of 8 groups of 4 ticks
  // (6 px apart) each ending in a longer white tick.
  /** @plan photo inputs: x */
  photo() {
    move(x, 13);
    line(x, 127, 65535);
    const x1 = x + 1;
    move(x1, 13);
    line(x1, 127, 63488);
    move(0, 13);
    line(239, 13, 50712);
    move(0, 127);
    line(239, 127, 50712);
    let t = 0;
    for (let k = 0; k < 8; k++) {
      for (let j = 0; j < 4; j++) {
        move(t, 128);
        line(t, 131, 50712);
        t += 6;
      }
      move(t, 128);
      line(t, 133, 65535);
      t += 6;
    }
  },

  // conf: confetti. Up to 96 flakes (input 1 of them) on fixed sine tracks,
  // drifting with the time (input 0); yellow and 129055 - yellow alternate.
  /** @plan conf inputs: t, count */
  conf() {
    let i = 0, colour = 65504;
    for (let j0 = 0; j0 < 96; j0++) {
      i += 1;
      if (i > count) break;
      const x = sin(i * 1.7) * 110 + 120;
      const y = sin(i * 2.1 + (i * .00041 + .037) * t) * 64 + 67;
      move(x, y);
      line(x + 1, y + 1 + 1, colour);
      colour = 129055 - colour;
    }
  },

  // silk: the jockey over the close-up horse (points baked for HX,HY; README):
  // the silk in colour, raised by bob px, and a white cap by CUBIC.
  /** @plan silk inputs: colour, bob */
  silk() {
    move(113, 96 + bob);
    line(116, 92 + bob, colour);
    line(109, 84 + bob, colour);
    line(119, 74 + bob, colour);
    line(127, 80 + bob, colour);
    const y0 = 71 + bob;
    const y1 = 66 + bob;
    const y2 = 66 + bob;
    const y3 = 71 + bob;
    cubic(5, 65535, 120, y0, 120, y1, 128, y2, 128, y3);
  },

  // map: the race map. The course line, the finish mark, and each horse's
  // x (inputs 0..7) as a 4 px dash in its silk (derby_watch.js SILK, baked).
  /** @plan map inputs: h0, h1, h2, h3, h4, h5, h6, h7 */
  map() {
    move(8, 12);
    line(232, 12, 16904);
    move(232, 2);
    line(232, 12, 63488);
    move(h0, 3);
    line(h0 + (-4), 3, 65535);
    move(h1, 4);
    line(h1 + (-4), 4, 35953);
    move(h2, 5);
    line(h2 + (-4), 5, 63488);
    move(h3, 6);
    line(h3 + (-4), 6, 9087);
    move(h4, 7);
    line(h4 + (-4), 7, 65504);
    move(h5, 8);
    line(h5 + (-4), 8, 2016);
    move(h6, 9);
    line(h6 + (-4), 9, 64800);
    move(h7, 10);
    line(h7 + (-4), 10, 63519);
  },

  // vis: the big screen. Its face filled row by row in the colour face (the
  // feed is drawn over it), bezel rings round it, a top light, and two pairs
  // of legs down to legY.
  /** @plan vis inputs: x0, y0, x1, y1, bezel, legY, face */
  vis() {
    let l = x0;
    let t = y0;
    let r = x1;
    let b = y1;
    const fl = l + 1;
    const fr1 = r + (-1);
    let row = t + 1;
    for (let k = 0; k < t * (-1) + b + (-1); k++) {
      move(fl, row);
      line(fr1, row, face);
      row += 1;
    }
    for (let k = 0; k < bezel; k++) {
      move(l, t);
      line(r, t, 10565);
      line(r, b, 10565);
      line(l, b, 10565);
      line(l, t, 10565);
      l += (-1);
      t += (-1);
      r += 1;
      b += 1;
    }
    move(l, t);
    line(r, t, 23275);
    const w = l * (-1) + r;
    const q1 = w * .25 + l;
    const q3 = q1 + w * .5;
    const nb = bezel * (-1);
    const a = q1 + nb;
    move(a, b);
    line(a, legY, 19049);
    const a2 = q1 + bezel;
    move(a2, b);
    line(a2, legY, 19049);
    const a3 = q3 + nb;
    move(a3, b);
    line(a3, legY, 19049);
    const a4 = q3 + bezel;
    move(a4, b);
    line(a4, legY, 19049);
  },

  // fr: a horse head on (HEAD ON cut): x, ground y, px per unit s, coat,
  // jockey (silk), lift of each foreleg (px).
  /** @plan fr inputs: x, ground, s, coat, jockey, lift0, lift1 */
  fr() {
    const k0 = (-1.2) * s + x;
    const k1 = k0 * (-1) + x + x;
    const knee = (-6) * s + ground;
    move(k0, knee);
    line(k0, lift0 * (-1) + ground, coat);
    move(k1, knee);
    line(k1, lift1 * (-1) + ground, coat);
    const ns = s * (-1);
    const c1 = s * (-1) + x;
    const c2 = x + s;
    const c0 = c1 + ns;
    const c3 = c2 + s;
    const chest = (-9.5) * s + ground;
    const belly = chest + s;
    move(c0, knee);
    line(c0, belly, coat);
    line(c1, chest, coat);
    line(c2, chest, coat);
    line(c3, belly, coat);
    line(c3, knee, coat);
    line(c0, knee, coat);
    const head = (-13) * s + ground;
    move(k0, chest);
    line(c1, head, jockey);
    line(c2, head, jockey);
    line(k1, chest, jockey);
    plot(x, head + ns, jockey);
    move(x, belly + s);
    line(x, chest + ns + ns, coat);
  },

  // nil: draws nothing; the code of the typed-point plans (gallop frames, hd).
  /** @plan nil inputs: */
  nil() {
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

  // pt: turf stripes in one green ($0; t0 and t1 are the two), each from the
  // far rail (farX, farZ past the near point) to the near one. The far 1/Z'
  // starts from the near one (4 steps), then 3 a stripe; the near 4.
  /** @plan pt inputs: x, dx, z, dz, stripes, r, farX, farZ */
  pt(green) {
    let px = x, pz = z, near = r;
    const fz0 = pz + farZ;
    const f1 = near * (fz0 * near + 2);
    const f2 = f1 * (fz0 * f1 + 2);
    const f3 = f2 * (fz0 * f2 + 2);
    let far = f3 * (fz0 * f3 + 2);
    for (let j = 0; j < stripes; j++) {
      const fz = pz + farZ;
      const g1 = far * (fz * far + 2);
      const g2 = g1 * (fz * g1 + 2);
      far = g2 * (fz * g2 + 2);
      move((px + farX) * far, far * 6 + 28);
      line(px * near, near * 6 + 28, green);
      px += dx;
      pz += dz;
      const n1 = near * (pz * near + 2);
      const n2 = n1 * (pz * n1 + 2);
      const n3 = n2 * (pz * n2 + 2);
      near = n3 * (pz * n3 + 2);
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
  }
};
// The close-up horse: 6 gallop frames of one polyline (typed points, half
// pixels), fixed on screen with the hip at HX,HY and 4 px per unit.
const HX = 92, HY = 88, HS = 4, bob = k => .35 * sin(2 * PI * k / 3);
// Units, y down, hip at 0,0; a pair 20+o,l is a leg of phase o and
// segment length l from the point before it (knee, hoof, back up).
const BODY = [-2.4, 3.2, -1.6, 1.4, -.6, 0, .3, -.4, 3.8, .3, 7, -.9, 8.3, -3.1, 9.9, -5, 9.8, -6, 10.5, -4.9,
  12.7, -2.3, 12.5, -1.6, 10.4, -2.4, 9.3, -2, 8.6, .8, 7.4, 2.2, 23.14, 1.9, 23.64, 1.9, 4, 2.7, 1.2, 2, 20, 2,
  20.5, 2, -.5, 1.2, -.6, 0];
let F = null;
// HEAD ON's fixed set (rails, finish line, stands, the screen edge on) as
// one polyline; points from the camera in README.
const HD = [[120, -155, 120, 425, 120, -155, -63, 323, 425, 120, 85, 159, 120, 100, 142, 120, 184, 166, 166, 166, 156, 156, 166, 166, 184, 184, 189, 189, 196, 196, 204, 204, 214, 214, 227, 227, 240, 227, 227, 240, 227, 227, 120, 184],
  [50, 105, 50, 105, 50, 160, 123, 123, 160, 50, 64, 64, 50, 58, 58, 50, 56, 56, 40, 14, 21, 42, 40, 56, 56, 19, 17, 56, 57, 14, 10, 58, 59, 5, -1, 60, 61, 60, -1, -8, -1, 60, 50, 19]];
const PAD = ['turf', 'rail', 'stands', 'silk', 'g0', 'g1', 'g2', 'g3', 'g4', 'g5', 'crowd', 'pole'],
  RUN = ['gate', 'r0', 'r1', 'r2', 'r3', 'r4', 'r5', 'r6', 'r7', 'map', 'vis', 'fr', 'hd', 'prail', 't0', 't1', 'hl'];
