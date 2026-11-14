'use strict';
// ---- The panning view (README "首振りカメラ"): pocket.derby holds the unit
// (aim()) and draws the series (main/pocket/pocket_derby.c,
// docs/apps/derby-ser-native.md); the JS gives depths and steps (Q16 m).
// Nothing deeper than 75 m past the leader (150 at LIGHT); posts 4 px apart
// at least (8 with the screen in view).
function pan(xs) {
  const k = KN[tier], L = vr ? 8 : 4;
  S.view((tier ? 75 : 150) * QM, t, live.hl, live.crowd, k[0], 163840);
  S.ser(live.prail, 40 * QM, 12 * QM, 0, L, 31727, -491520);
  if (vr) {
    S.part(6, live.hl, von < 8 ? 0 : von < 11 ? 0x632c : 0x0866);
    feed(xs, mn((vr[2] - vr[0]) * 546, (vr[3] - vr[1] - 1) * 2185));
  }
  S.ser(live.prail, 1481114, k[4] * QM, 0, L, 0xad55, 321126);
  S.ser(live.t0, 720896, 2 * k[5] * QM, 0, L, 760218);
  S.ser(live.t1, 720896, 2 * k[5] * QM, k[5] * QM, L, 760218);
  S.part(7, live.pole);
  runners(xs, 1, -1);
  S.ser(live.prail, 720896, k[4] * QM, 0, L, 0xffff, 321126);
}
