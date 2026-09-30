// DERBY WATCH's 13 procedural plans as JS functions (tools/kasane_ir/plan_js.mjs
// compiles them; docs/kasane/js-to-ir.md section 5). Converted from
// tools/kasane_ir/plans/*.kjs; the IR must equal theirs (check_js.py).
'use strict';

// rail: the running rail. Posts from the top rail to the ground, a top and
// a mid rail from the first post to one post past the last.
/** @plan rail inputs: x0, dx, top, ground, posts, mid, colour */
function rail() {
  let x = x0;
  const end = posts * dx + x;
  move(x, top); line(end, top, colour);
  move(x, mid); line(end, mid, colour);
  for (let j0 = 0; j0 < posts; j0++) {
    move(x, top); line(x, ground, colour);
    x += dx;
  }
}

// turf: mowing stripes from the near edge (x0, y0) to the far edge (x1, y1),
// alternating between two greens (512 + 288 * phase, then 1312 minus it).
/** @plan turf inputs: x0, x1, dx0, dx1, y0, y1, stripes, phase */
function turf() {
  let near = x0, far = x1, green = 512 + phase * 288;
  for (let j0 = 0; j0 < stripes; j0++) {
    move(near, y0); line(far, y1, green);
    near += dx0; far += dx1;
    green = 1312 - green;
  }
}

/** @plan stands inputs: in0, in1, in2, in3, in4, in5, in6, in7 */
function stands(p0, p1, p2, p3) {
  let r8 = in0;
  let r1 = in2 + 0;
  for (let j0 = 0; j0 < p0; j0++) {
    move(0, r1);
    line(239, r1, 21130);
    r1 += in3;
  }
  const r3b = in7 + in3 + in3;
  const r5 = r3b + 0;
  for (let j0 = 0; j0 < in4; j0++) {
    const r0b = r8 + 0;
    const r2b = r8 + 0;
    const r4 = r8 + in1;
    const r6 = r8 + in1;
    move(r8, in2);
    line(r8, in7, 31727);
    cubic(p3, 50712, r0b, in7, r2b, r3b, r4, r5, r6, in7);
    r8 += in1;
  }
}

// crowd: the stand's spectators as dots. $1 rows of in(4) bays of $2 dots,
// each dot swaying on its own phase; colours cycle through two values
// (105642 minus the last) seeded by input 6.
/** @plan crowd inputs: x0, spread, y0, rowGap, bays, sway, seed */
function crowd(p0, p1, p2, p3, p4, p5, p6) {
  let colour = seed * 12650 + 46496;
  const step = p6 * spread;
  let rowPhase = 0;
  let y = rowGap * .5 + y0;
  for (let j0 = 0; j0 < p1; j0++) {
    let x = x0;
    let phase = sway + rowPhase;
    for (let j1 = 0; j1 < bays; j1++) {
      for (let j2 = 0; j2 < p2; j2++) {
        plot(sin(phase) * 1.5 + x, y, colour);
        x += step;
        phase += 2.39996;
        colour = 105642 - colour;
      }
    }
    y += rowGap;
    rowPhase += .9;
  }
}

// runner: one horse on the gate / FIELD shots. Hip at (x, y), size s; the
// body in coat $0, four legs to (leg0..3, hoof), the jockey in silk $1 and
// a cap in $2 (silk ^ 0x8410).
/** @plan runner inputs: x, y, s, hoof, leg0, leg1, leg2, leg3 */
function runner(p0, p1, p2) {
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
}

/** @plan gate inputs: in0, in1, in2, in3, in4 */
function gate() {
  let r0 = in0;
  let r1 = in1;
  let r2 = in2;
  for (let j0 = 0; j0 < 9; j0++) {
    const r6 = 120 + r0;
    const r8 = in4 + r2;
    line(r6, r8, 40147);
    line(r6, in4 + r1, 40147);
    move(r6, r8);
    r0 *= in3;
    r1 *= in3;
    r2 *= in3;
  }
}

/** @plan pole inputs: in0, in1, in2, in3, in4, in5, in6, in7 */
function pole() {
  move(in0, in1);
  line(in0, in2, 63488);
  const r12 = in3 * (-1);
  const r13 = in2 + r12;
  const r14b = (-1.33) * in3;
  const r0 = in0 + r12;
  const r1 = r13 + 0;
  const r2 = r0 + 0;
  const r3 = r13 + r14b;
  const r4 = in0 + in3;
  const r5 = r3 + 0;
  const r6 = r4 + 0;
  const r7 = r13 + 0;
  cubic(6, 65535, r0, r1, r2, r3, r4, r5, r6, r7);
  const r14c = r14b * (-1);
  const r3b = r13 + r14c;
  const r5b = r13 + r14c;
  cubic(6, 65535, r0, r1, r2, r3b, r4, r5b, r6, r7);
  move(in4, in5);
  line(in6, in7, 65535);
  move(in4 + 1, in5);
  line(in6 + 1, in7, 65535);
}

/** @plan photo inputs: in0 */
function photo() {
  move(in0, 13);
  line(in0, 127, 65535);
  const r0b = in0 + 1;
  move(r0b, 13);
  line(r0b, 127, 63488);
  move(0, 13);
  line(239, 13, 50712);
  move(0, 127);
  line(239, 127, 50712);
  let r4 = 0;
  for (let j0 = 0; j0 < 8; j0++) {
    for (let j1 = 0; j1 < 4; j1++) {
      move(r4, 128);
      line(r4, 131, 50712);
      r4 += 6;
    }
    move(r4, 128);
    line(r4, 133, 65535);
    r4 += 6;
  }
}

// conf: confetti. Up to 96 flakes (input 1 of them) on fixed sine tracks,
// drifting with the time (input 0); yellow and 129055 - yellow alternate.
/** @plan conf inputs: t, count */
function conf() {
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
}

/** @plan silk inputs: in0, in1 */
function silk() {
  move(113, 96 + in1);
  line(116, 92 + in1, in0);
  line(109, 84 + in1, in0);
  line(119, 74 + in1, in0);
  line(127, 80 + in1, in0);
  const r1f = 71 + in1;
  const r3d = 66 + in1;
  const r5d = 66 + in1;
  const r7d = 71 + in1;
  cubic(5, 65535, 120, r1f, 120, r3d, 128, r5d, 128, r7d);
}

/** @plan map inputs: in0, in1, in2, in3, in4, in5, in6, in7 */
function map() {
  move(8, 12);
  line(232, 12, 16904);
  move(232, 2);
  line(232, 12, 63488);
  move(in0, 3);
  line(in0 + (-4), 3, 65535);
  move(in1, 4);
  line(in1 + (-4), 4, 35953);
  move(in2, 5);
  line(in2 + (-4), 5, 63488);
  move(in3, 6);
  line(in3 + (-4), 6, 9087);
  move(in4, 7);
  line(in4 + (-4), 7, 65504);
  move(in5, 8);
  line(in5 + (-4), 8, 2016);
  move(in6, 9);
  line(in6 + (-4), 9, 64800);
  move(in7, 10);
  line(in7 + (-4), 10, 63519);
}

/** @plan vis inputs: in0, in1, in2, in3, in4, in5, in6 */
function vis() {
  let r0 = in0;
  let r1 = in1;
  let r2 = in2;
  let r3 = in3;
  const r8 = r0 + 1;
  const r9 = r2 + (-1);
  let r10 = r1 + 1;
  for (let j0 = 0; j0 < r1 * (-1) + r3 + (-1); j0++) {
    move(r8, r10);
    line(r9, r10, in6);
    r10 += 1;
  }
  for (let j0 = 0; j0 < in4; j0++) {
    move(r0, r1);
    line(r2, r1, 10565);
    line(r2, r3, 10565);
    line(r0, r3, 10565);
    line(r0, r1, 10565);
    r0 += (-1);
    r1 += (-1);
    r2 += 1;
    r3 += 1;
  }
  move(r0, r1);
  line(r2, r1, 23275);
  const r8c = r0 * (-1) + r2;
  const r9d = r8c * .25 + r0;
  const r10b = r9d + r8c * .5;
  const r11e = in4 * (-1);
  const r12 = r9d + r11e;
  move(r12, r3);
  line(r12, in5, 19049);
  const r12b = r9d + in4;
  move(r12b, r3);
  line(r12b, in5, 19049);
  const r12c = r10b + r11e;
  move(r12c, r3);
  line(r12c, in5, 19049);
  const r12d = r10b + in4;
  move(r12d, r3);
  line(r12d, in5, 19049);
}

/** @plan fr inputs: in0, in1, in2, in3, in4, in5, in6 */
function fr() {
  const r3c = (-1.2) * in2 + in0;
  const r4c = r3c * (-1) + in0 + in0;
  const r7c = (-6) * in2 + in1;
  move(r3c, r7c);
  line(r3c, in5 * (-1) + in1, in3);
  move(r4c, r7c);
  line(r4c, in6 * (-1) + in1, in3);
  const r14 = in2 * (-1);
  const r11b = in2 * (-1) + in0;
  const r12 = in0 + in2;
  const r10 = r11b + r14;
  const r13 = r12 + in2;
  const r5f = (-9.5) * in2 + in1;
  const r6d = r5f + in2;
  move(r10, r7c);
  line(r10, r6d, in3);
  line(r11b, r5f, in3);
  line(r12, r5f, in3);
  line(r13, r6d, in3);
  line(r13, r7c, in3);
  line(r10, r7c, in3);
  const r7f = (-13) * in2 + in1;
  move(r3c, r5f);
  line(r11b, r7f, in4);
  line(r12, r7f, in4);
  line(r4c, r5f, in4);
  plot(in0, r7f + r14, in4);
  move(in0, r6d + in2);
  line(in0, r5f + r14 + r14, in3);
}
