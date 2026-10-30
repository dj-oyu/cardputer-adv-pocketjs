// DERBY's panning rail (prail) and turf stripes (pt) rewritten for Shiki's
// fixed point (docs/kasane/shiki.md sec.5.5, 5.6). Not shipped: the app runs
// apps/derby/derby_prog.js. tools/kasane_ir/q16_error.mjs --alt runs these in
// float32 and Q16.16 and compares them with the originals in double.
//
// The one change: depth (z, dz, farZ) arrives 256 times larger
// (@inputScale; the caller multiplies), so a depth of 0.05 is 12.8, not
// 3,277 steps of 1/65536 but 838,861, and the depth step dz, as small as
// 0.0005, keeps a relative rounding of 1/16,000 instead of 1/70. 1/Z' (q,
// 1.2..18 in the game) stays as it was. Newton's step for q = 1/(-pz),
// q (2 + pz q), becomes q (2 + PZ q / 256): PZ q is about -256, and the
// division by 256 is an exact shift in fixed point and in double, so in
// double these equal the originals.

/** @plan prail inputs: x, dx, z, dz, posts, r, colour, top
 *  @inputScale z=256, dz=256 */
function prail() {
  let px = x, pz = z, q = r;
  const x0 = px * q;
  for (let j = 0; j < posts; j++) {
    const sx = px * q;
    move(sx, q * top + 28);
    line(sx, q * 6 + 28, colour);
    px += dx;
    pz += dz;
    const a = q * (pz * q / 256 + 2);
    const b = a * (pz * a / 256 + 2);
    q = b * (pz * b / 256 + 2);
  }
  const x1 = px * q;
  move(x0, r * top + 28);
  line(x1, q * top + 28, colour);
  const mid = (top + 6) * .5;
  move(x0, r * mid + 28);
  line(x1, q * mid + 28, colour);
}

/** @plan pt inputs: x, dx, z, dz, stripes, r, farX, farZ
 *  @inputScale z=256, dz=256, farZ=256 */
function pt(green) {
  let px = x, pz = z, near = r;
  const fz0 = pz + farZ;
  const f1 = near * (fz0 * near / 256 + 2);
  const f2 = f1 * (fz0 * f1 / 256 + 2);
  const f3 = f2 * (fz0 * f2 / 256 + 2);
  let far = f3 * (fz0 * f3 / 256 + 2);
  for (let j = 0; j < stripes; j++) {
    const fz = pz + farZ;
    const g1 = far * (fz * far / 256 + 2);
    const g2 = g1 * (fz * g1 / 256 + 2);
    far = g2 * (fz * g2 / 256 + 2);
    move((px + farX) * far, far * 6 + 28);
    line(px * near, near * 6 + 28, green);
    px += dx;
    pz += dz;
    const n1 = near * (pz * near / 256 + 2);
    const n2 = n1 * (pz * n1 / 256 + 2);
    const n3 = n2 * (pz * n2 / 256 + 2);
    near = n3 * (pz * n3 / 256 + 2);
  }
}
