'use strict';
// The runners' JS path from before pocket.derby.runners, loaded by the first
// up key (frame() in derby_play.js), which then flips AB: the A/B on the
// device in one binary (docs/apps/derby-runners-native.md).
const BH = new Uint32Array(32), BI = new Int32Array(32 * 7), PO = new Int32Array(32), HO = new Int32Array(24);
let bn = 0, bs = 7;
function bput(h, a, b, c, d, e, f, g) { const o = bn * bs; BH[bn++] = h; BI[o] = a; BI[o + 1] = b; BI[o + 2] = c; BI[o + 3] = d; BI[o + 4] = e; BI[o + 5] = f; if (bs > 6) BI[o + 6] = g; }
function bflush() { if (bn) SK.drawMany(BH.subarray(0, bn), BI.subarray(0, bn * bs), bs); bn = 0; }
function rin(l, X, S, Y, d) {
  if (!live.run || !live.legs) return;
  bs = 7; const c = F.h[l].coat;
  bput(live.legs, X, S, Y, d, c, 0, 0);
  bput(live.run, X, S, Y, d, c, SILK[l], SILK[l] ^ 0x8410);
}
function abRun(xs, pan, close, K) {
  if (pan) {
    S.lanes(xs, lz, PO, 1);
    zo.sort((a, b) => PO[4 * b + 3] - PO[4 * a + 3]);
    for (const l of zo) {
      const X = PO[4 * l];
      if (PO[4 * l + 3] && X > -10485760 && X < 26214400) rin(l, X, PO[4 * l + 1], PO[4 * l + 2], DO(xs[l], l));
    }
    return bflush();
  }
  S.lanes(xs, lz, PO, 0);
  const L = K ? K[0] * QM : -160 * QM, R = K ? (K[2] - 1) * QM : 400 * QM;
  for (const l of zo) {
    if (K && ro.indexOf(l) >= FN[tier]) continue;
    const X = PO[4 * l], s = PO[4 * l + 1];
    if (l === close) {
      bflush();
      const g = ((DO(xs[l], l) >> 12) * 60495 >> 20) % 6;
      dr('g' + g, []);
      dr('silk', SILK[l], HS * bob(g) + 128 >> 8);
    } else if (K ? X - (5 * s >> 1) >= L && X + (25 * s >> 1) <= R : X > L && X < R) rin(l, X, s, PO[4 * l + 2], DO(xs[l], l));
  }
  bflush();
}
function abHead(xs) {
  bs = 6;
  S.head(xs, lz, HO);
  for (let i = 7; i >= 0; --i) {
    const l = ro[i], o = 3 * l, d = DO(xs[l], l), c = F.h[l].coat;
    bput(live.frl, HO[o], HO[o + 1], HO[o + 2], d, c, SILK[l]);
    bput(live.frb, HO[o], HO[o + 1], HO[o + 2], d, c, SILK[l]);
  }
  bflush();
}
