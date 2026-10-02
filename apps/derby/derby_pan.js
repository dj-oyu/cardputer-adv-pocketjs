'use strict';
// ---- The panning view (README "首振りカメラ"). A world point [x, z] in pc's
// view: [X'', Z'], screen x = X''/Z', px per m 1/Z'.
function pj(P) {
  const a = P[0] - pc[0], b = P[1] - pc[1], Z = (a * pc[2] + b * pc[3]) / pc[4];
  return [a * pc[3] - b * pc[2] + 120 * Z, Z];
}
// The series along the course (柵・芝・スタンド・観客) run in C:
// pocket.derby (main/pocket/pocket_derby.c, docs/apps/derby-ser-native.md).
const S = pocket.derby;
// One frame from pc: stands (pillars, roof, tiers), crowd, the screen, far
// rail, turf, poles, runners, near rail. Nothing deeper than 75 m past the
// leader (150 at LIGHT); posts 4 px apart at least (8 with the screen in view).
function pan(xs) {
  const k = KN[tier], zf = pc[5] + (tier ? 75 : 150), L = vr ? 8 : 4;
  S.view(pc, zf, t, live.hl, live.crowd, k[0], KN[3][5]);
  S.ser(live.prail, 40, 12, 0, L, 31727, -7.5);
  if (vr) {
    const m = M.ceil(10 * mx(vq[1], vq[3])) + 1;
    dr('hl', vq[0], vq[1], vq[2], vq[3], 0, -10 / (m - 1), m, von < 8 ? 0 : von < 11 ? 0x632c : 0x0866);
    dr('hl', vq[0], vq[1], vq[2], vq[3], 0, -10, 2, 10565);
    feed(xs, mn((vr[2] - vr[0]) / 120, (vr[3] - vr[1] - 1) / 30));
  }
  S.ser(live.prail, DFR, k[4], 0, L, 0xad55, 4.9);
  S.ser(live.t0, DNR, 2 * k[5], 0, L, 11.6);
  S.ser(live.t1, DNR, 2 * k[5], k[5], L, 11.6);
  for (let m = 200; m <= D; m += 200) {
    const p = pj(pose(m, DFR)), r = 1 / p[1], x = p[0] * r, e = m === D, n = e ? pj(pose(m, DNR)) : p, y = n[0] / n[1];
    if (p[1] > .02 && n[1] > .02 && x > -40 && x < 280 && y > -400 && y < 640)
      dr('pole', x, 28 + 6 * r, 28 + (e ? 2 : 3.4) * r, (e ? .55 : .3) * r, y, 28 + 6 / n[1], x, 28 + 6 * r);
  }
  for (const l of zo) {
    const p = pj(pose(xs[l] - 12.5 * U, lz[l])), r = 1 / p[1], X = p[0] * r;
    if (p[1] > .02 && X > -160 && X < 400) rin(l, X, r * U, 28 + 6 * r, ph[l]);
  }
  S.ser(live.prail, DNR, k[4], 0, L, 0xffff, 4.9);
}
