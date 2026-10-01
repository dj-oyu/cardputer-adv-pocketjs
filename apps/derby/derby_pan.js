'use strict';
// ---- The panning view (README "首振りカメラ"). A world point [x, z] in pc's
// view: [X'', Z'], screen x = X''/Z', px per m 1/Z'.
function pj(P) {
  const a = P[0] - pc[0], b = P[1] - pc[1], Z = (a * pc[2] + b * pc[3]) / pc[4];
  return [a * pc[3] - b * pc[2] + 120 * Z, Z];
}
// Narrows Lo..Hi to c0 + c1 g >= 0.
function lim(c0, c1) { if (c1 > 0) Lo = mx(Lo, -c0 / c1); else if (c1 < 0) Hi = mn(Hi, -c0 / c1); else if (c0 < 0) Hi = -1e9; }
// Plan n along the course at depth w, a point every s m on g0's grid, chord
// by chord (VC: the chords in view, ends on the grid), in view (x -40..280,
// depth .02f..zf) and one point past each end of the view, far to near: the
// VM finds each 1/Z' by Newton from the last one, which is farther, so it
// rises to it and cannot diverge. Strides double toward the far end where
// points close to L px, but never a step of more than .3 of the depth
// (Newton's error, 3 steps: .3^8), the halves on the grid. Inputs [X'',
// dX'', -Z', -dZ', count, 1/Z', a, b]; prail's lines reach the point after
// the last (the next draw's first, or the one past the near end).
// L 0: the crowd (pk), no LOD, its first column on row 0 of the checkerboard
// and a its phase (a dot's is fixed on the course: pk adds 2.39996 a column).
// Turf (t0, t1): a is the far rail's distance, a, b its offset [X'', -Z'] off
// the chord's normal. The stands (b < 0): their tiers chord by chord, hl's
// lines across the chord's view from the ground up, 2.4 m apart.
function ser(n, w, s, g0, L, zf, a, b) {
  let P, pg;
  for (let i = 0; i < VC.length; ++i) {
    const j = VC[i];
    if (VE && ou(j, w, s, zf)) continue;
    const ga = g0 + rnd((CH[j] - g0) / s) * s, gb = g0 + rnd((CH[j + 1] - g0) / s) * s, A = ga === pg ? P : pose(ga, w), B = P = pose(pg = gb, w),
      dx = (B[0] - A[0]) / (gb - ga), dz = (B[1] - A[1]) / (gb - ga), X = A[0] + (g0 - ga) * dx - pc[0], Y = A[1] + (g0 - ga) * dz - pc[1],
      Q = (X * pc[2] + Y * pc[3]) / pc[4], q = [X * pc[3] - Y * pc[2] + 120 * Q, Q], u = (dx * pc[2] + dz * pc[3]) / pc[4], v = dx * pc[3] - dz * pc[2] + 120 * u;
    Lo = ga - g0; Hi = gb - g0;
    lim(q[1] - .02, u); lim(zf / pc[4] - q[1], -u); lim(q[0] + 40 * q[1], v + 40 * u); lim(280 * q[1] - q[0], 280 * u - v);
    if (!(Lo < Hi)) continue;
    let c0 = a, c1 = b;
    if (n[0] === 't') {
      const l = a / M.sqrt(dx * dx + dz * dz), x = -dz * l, z = dx * l;
      c1 = -(x * pc[2] + z * pc[3]) / pc[4]; c0 = x * pc[3] - z * pc[2] - 120 * c1;
    }
    const o = u < 0 ? -1 : 1, U = u * o, V = v * o, t1 = o > 0 ? Hi : -Lo;
    let k = s, Z = M.sqrt(M.abs(v * q[1] - q[0] * u) * s / L), n0 = flo((o > 0 ? Lo : -Hi) / s) * s, e;
    while (Z < q[1] + t1 * U && 2 * k * U < .3 * (q[1] + t1 * U) && k < 64 * s) k *= 2, Z *= M.SQRT2;
    e = M.ceil(t1 / k) * k;
    if (!inr(q, U, V, e)) e -= k;
    if (!inr(q, U, V, n0)) n0 += s;
    if (!L && (rnd(o * e / s) + t) & 1) e -= s;
    for (;;) {
      Z /= M.SQRT2;
      const h = k > s ? M.ceil(mx(U > 1e-7 ? (mx(Z, k * U / .3) - q[1]) / U : -1e9, n0) / k) * k : n0, c = rnd((e - h) / k), z = q[1] + e * U;
      if (c > 0) dr(n, [q[0] + e * V, -V * k, -z, U * k, mn(255, c), 1 / z, L ? c0 : -e / s * 2.39996 % (2 * PI) - 2 * PI * rnd(c * .191) + 1.8, c1]);
      if (k === s) break;
      e = mn(e, h); k /= 2;
    }
    if (b < 0) {
      const y = q[1] + Lo * u, z = q[1] + Hi * u;
      dr('hl', [(q[0] + Lo * v) / y, 1 / y, (q[0] + Hi * v) / z, 1 / z, 6, -2.4, KN[tier][0], 21130]);
    }
  }
}
// Chord j (VC, VE) at depth w wholly out of view, by m m at least: both
// ends behind the near or past the far depth, or left or right of the view.
function ou(j, w, m, zf) {
  const a = 4 * j, f = pc[4], k = m * M.sqrt(f * f + 25600), l = VE[a] + w * VE[a + 2], d = VE[a + 1] + w * VE[a + 3],
    L = VE[a + 4] + w * VE[a + 6], D = VE[a + 5] + w * VE[a + 7];
  return d < .02 * f - m && D < .02 * f - m || d > zf + m && D > zf + m || 160 * d + l * f < -k && 160 * D + L * f < -k ||
    160 * d - l * f < -k && 160 * D - L * f < -k;
}
function inr(q, U, V, e) {
  const z = q[1] + e * U, x = (q[0] + e * V) / z;
  return z > .02 && x > -400 && x < 640;
}
// One frame from pc: stands (pillars, roof, tiers), crowd, the screen, far
// rail, turf, poles, runners, near rail. Nothing deeper than 75 m past the
// leader (150 at LIGHT); posts 4 px apart at least (8 with the screen in view).
// VE: each chord end (CH) from pc, [across, along the view] at w 0 and per m
// of w; VC: the chords whose band (w 11..40) may be in view (ou()). The
// straight is one chord, always in view.
function pan(xs) {
  const k = KN[tier], zf = pc[5] + (tier ? 75 : 150), L = vr ? 8 : 4;
  VE = CRS === OC && []; VC = VE ? [] : [0];
  if (VE) {
    for (let a = 0; a < 4 * CH.length; a += 4) {
      const x = CP[a] - pc[0], z = CP[a + 1] - pc[1], c = CP[a + 2], d = CP[a + 3];
      VE.push(x * pc[3] - z * pc[2], x * pc[2] + z * pc[3], -d * pc[3] - c * pc[2], c * pc[3] - d * pc[2]);
    }
    for (let j = 1; j < CH.length; ++j) if (!ou(j - 1, 25.5, 30, zf)) VC.push(j - 1);
  }
  ser('prail', 40, 12, 0, L, zf, 31727, -7.5);
  ser('pk', 40, 12 / k[2], 0, 0, zf, 0, 46496 + 12650 * ((t >> 3 ^ t) & 1));
  if (vr) {
    const m = M.ceil(10 * mx(vq[1], vq[3])) + 1;
    dr('hl', vq.concat(0, -10 / (m - 1), m, von < 8 ? 0 : von < 11 ? 0x632c : 0x0866));
    dr('hl', vq.concat(0, -10, 2, 10565));
    feed(xs, mn((vr[2] - vr[0]) / 120, (vr[3] - vr[1] - 1) / 30));
  }
  ser('prail', DFR, k[4], 0, L, zf, 0xad55, 4.9);
  ser('t0', DNR, 2 * k[5], 0, L, zf, 11.6);
  ser('t1', DNR, 2 * k[5], k[5], L, zf, 11.6);
  for (let m = 200; m <= D; m += 200) {
    const p = pj(pose(m, DFR)), r = 1 / p[1], x = p[0] * r, e = m === D, n = e ? pj(pose(m, DNR)) : p, y = n[0] / n[1];
    if (p[1] > .02 && n[1] > .02 && x > -40 && x < 280 && y > -400 && y < 640)
      dr('pole', [x, 28 + 6 * r, 28 + (e ? 2 : 3.4) * r, (e ? .55 : .3) * r, y, 28 + 6 / n[1], x, 28 + 6 * r]);
  }
  for (let l = 7; l >= 0; --l) {
    const p = pj(pose(xs[l] - 12.5 * U, DL[l])), r = 1 / p[1], X = p[0] * r;
    if (p[1] > .02 && X > -160 && X < 400) rin(l, X, r * U, 28 + 6 * r, ph[l]);
  }
  ser('prail', DNR, k[4], 0, L, zf, 0xffff, 4.9);
  VE = VC = 0;
}
