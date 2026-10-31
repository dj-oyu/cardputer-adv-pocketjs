// DERBY's panning rail (prail) and turf stripes (pt), and the mowing stripes
// (turf), as @shiki would take them (docs/kasane/shiki-spec.md sec.8.3,
// shiki.md sec.5.6). Not shipped:
// the app runs apps/derby/derby_prog.js. tools/kasane_ir/q16_error.mjs --alt
// runs these under the spec's fixed point and compares them with the
// originals in double.
//
// The functions are the shipped ones, unchanged; only `@q` is added. Depth
// (z, dz, farZ and the values made from them) is small (z -0.8..-0.05, dz
// down to 0.0005 in the game), so it is declared Q24: 1/16,777,216 steps,
// up to +-128. 1/Z' (r, q, near, far: 1.2..18) stays Q16. Newton's step
// q (pz q + 2) then multiplies a Q24 by a Q16 into Q16 with MUL s = 24: the
// 256 the first rewrite divided by by hand is that shift.

/** @plan prail inputs: x, dx, z, dz, posts, r, colour, top
 *  @q z=24, dz=24, pz=24 */
function prail() {
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
}

/** @plan pt inputs: x, dx, z, dz, stripes, r, farX, farZ
 *  @q z=24, dz=24, farZ=24, pz=24, fz0=24, fz=24 */
function pt(green) {
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
}

// turf: the shipped function with @q phase=0. green is a colour (Q0, rule
// 1), and 512 + phase * 288 gives it a Q16 input otherwise, which rule 9
// refuses: the writer says that phase is a whole number (0 or 1).
/** @plan turf inputs: x0, x1, dx0, dx1, y0, y1, stripes, phase
 *  @q phase=0 */
function turf() {
  let near = x0, far = x1, green = 512 + phase * 288;
  for (let j0 = 0; j0 < stripes; j0++) {
    move(near, y0); line(far, y1, green);
    near += dx0; far += dx1;
    green = 1312 - green;
  }
}
