'use strict';
// ---- Scenes: pad (pick), gate (runner plans load one a frame: horses
// enter the stalls), race, photo (still), res (result).
function enter(s) {
  scene = s; t = 0; need = 1;
  if (s === 'pad') { drop(['conf', 'band']); want(PAD); replay = 0; F = field((rng(HW ^ M.imul(sn, 0x9e3779b9))() * 4294967296 + M.imul(raceNo, 0x9e3779b9)) >>> 0); log('ODDS ' + (od = odds(F.h, F.o)) + (F.o ? ' OVAL' : ''));
    CRS = F.o ? OC : SC; CH = [-1e4];
    if (F.o) for (let j = 0; j <= PAN[3]; ++j) CH.push(OB + j * 120 * PI / PAN[3]);
    CH.push(1e4);
  }
  if (s === 'gate') { drop(['conf']); want(RUN); rs = race(F); ph = [0, 1, 2, 3, 4, 5, 6, 7]; cam = disp = slow = cm = hold = dl = man = von = 0; ld = -1; notes = FANFARE.slice(); }
  if (s === 'race') notes = BELL.slice();
  if (s === 'photo') { drop(['band']); want(['photo']); }
  if (s === 'res') { drop(RUN); drop(['photo']); want(['conf']); }
  log('SCENE ' + s + ' race=' + raceNo + ' seed=' + hex(F.seed) + ' plans=' + Object.keys(live).length + ' reg=' + reg);
}
const TX = [[[3, 1, 237, 12, 0xfffb96ff, 48], [153, 29, 237, 40, -1, 24], [153, 77, 237, 88, 0xc0d8ffff, 24],
  [3, 122, 237, 134, 0xfffb96ff, 60]], [[3, 123, 150, 134, -1, 24], [180, 123, 237, 134, 0xfffb96ff, 8],
  [170, 13, 237, 24, 0x01cdfeff, 12], [20, 36, 220, 58, 0xfffb96ff, 16, 'display'], [20, 58, 220, 70, -1, 24]],
  [[4, 1, 236, 12, 0xfffb96ff, 48], [4, 12, 236, 23, 0xd8e8ffff, 48], [4, 23, 236, 34, 0xd8e8ffff, 48],
    [4, 122, 236, 134, 0x05ffa1ff, 48]]];
function build(up) {
  V.replace(tx => {
    const pad = scene === 'pad', res0 = scene === 'res', tt = (b, s) => tx.text({bounds: b.slice(0, 4), text: s || '',
      font: b[6] || 'caption', color: b[4] < 0 ? 0xffffffff : b[4], capacity: b[5] || 8});
    tx.background(255);
    R = [tx.image({resource: res, bounds: [0, 0, 240, 135], sourceWidth: 240, sourceHeight: 135})];
    // The screen's lettering (metres to go, first three, LIVE and its lamp)
    // follows the face: whole-panel clips, since setRect keeps the clip.
    if (!pad && !res0) {
      R.v = [tt([0, 0, 1, 1, 0xffb030ff, 12]), tt([0, 0, 1, 1, -1, 4], 'LIVE'), tx.rect({bounds: [0, 0, 1, 1], color: 255})];
      for (let i = 0; i < 3; ++i) R.v[i].setClip(tx, [0, 0, 240, 135]);
    }
    if (pad || res0) tx.rect({bounds: [0, 0, 240, pad ? 25 : 36], color: 0x000820c8});
    if (pad) {
      tx.rect({bounds: [150, 28, 238, 90], color: 0x00081890});
      R.sel = tx.rect({bounds: [0, 13, 30, 24], color: 0x3050a0ff});
      ['SPD', 'STA', 'KCK'].forEach((s, i) => tt([153, 41 + 12 * i, 180, 52 + 12 * i, 0xc0d8ffff], s));
      R.bar = [0, 1, 2].map(i => tx.rect({bounds: [180, 43 + 12 * i, 181, 49 + 12 * i], color: 0x05ffa1ff}));
      // The tote: a silk swatch per runner, then its odds.
      R.od = SILK.map((c, i) => (tx.rect({bounds: [3 + 30 * i, 15, 6 + 30 * i, 22], color: (c >> 8 & 248) * 16777216 + (c >> 3 & 252) * 65536 + (c << 3 & 248) * 256 + 255}),
        tt([8 + 30 * i, 13, 32 + 30 * i, 24, 0xd8e8ffff, 4])));
    }
    R.t = TX[pad ? 0 : res0 ? 2 : 1].map(b => tt(b));
    // The curtain: a black rect over everything but DEMO.
    R.fd = tx.rect({bounds: [0, 0, 240, 135], color: 0});
    R.dm = tt([212, 109, 238, 120, 0xfffb96ff, 4], 'DEMO');
    up(tx);
  });
}
function hud(tx) {
  let s;
  if (scene === 'pad') {
    const h = F.h[pick];
    s = [(dm ? '' : 'RACE ' + raceNo + '  ') + '1000M ' + (F.o ? 'OVAL' : 'STRAIGHT') + '  SEED ' + hex(F.seed), num(pick) + ' ' + h.n,
      STY[h.sty] + ' x' + od[pick], 'BET ' + stake + '  PTS ' + pts + '   A/D HORSE E/S BET 1 GO'];
    for (let i = 0; i < 8; ++i) R.od[i].setText(tx, od[i] < 10 ? od[i].toFixed(1) : '' + rnd(od[i]));
    R.sel.setRect(tx, [1 + 30 * pick, 13, 31 + 30 * pick, 24]);
    const v = [(h.top - TOP) / SPR, (h.st - ST0) / ST1, (h.kick - KI0) / KI1];
    for (let i = 0; i < 3; ++i) R.bar[i].setRect(tx, [180, 43 + 12 * i, 182 + rnd(52 * mx(0, mn(1, v[i]))), 49 + 12 * i]);
  } else if (scene === 'res') {
    const o = fin.o, p = o.indexOf(pick);
    s = [(replay ? 'REPLAY  ' : 'WINNER  ') + num(o[0]) + ' ' + F.h[o[0]].n + '  ' + fin.mg,
      '1ST ' + (o[0] + 1) + '  2ND ' + (o[1] + 1) + '  3RD ' + (o[2] + 1) + '  4TH ' + (o[3] + 1),
      'YOUR ' + num(pick) + ' ' + th(p) + (replay ? '' : '  ' + (fin.dp < 0 ? '' : '+') + fin.dp),
      'PTS ' + pts + '   1 NEXT RACE   R REPLAY'];
  } else {
    const o = scene === 'photo' ? order(rs) : ro, lead = rs ? mx.apply(null, rs.x) : 0, ph2 = scene === 'photo';
    s = [(o[0] + 1) + '-' + (o[1] + 1) + '-' + (o[2] + 1) + '   ' + num(pick) + ' ' + th(o.indexOf(pick)),
      scene === 'gate' ? 'GATE' : mx(0, rnd(D - lead)) + 'M', camT > 0 ? NAMES[cm] : '',
      ph2 ? (t < 50 ? (t & 8 ? 'PHOTO' : '') : num(fin.o[0])) : '', ph2 && t >= 50 ? fin.mg : ''];
  }
  for (let i = 0; i < s.length; ++i) if (R.t[i].s !== s[i]) R.t[i].setText(tx, R.t[i].s = s[i]);
}
// The runners u of the way through the last sim step.
function at(u) {
  const a = [];
  for (let i = 0; i < 8; ++i) a[i] = rs.px[i] + (rs.x[i] - rs.px[i]) * u;
  return a;
}
// Finish: run the rest of the field unseen, then settle the bet.
function settle() {
  while (rs.done < 8 && rs.t < 200) step(F, rs, DT);
  const o = order(rs), a = rs.tc[o[1]] - rs.tc[o[0]], m = a * rs.v[o[1]];
  const mg = m < .02 ? 'DEAD HEAT' : m < .12 ? 'NOSE' : m < .3 ? 'SHORT HEAD' : m < .6 ? 'HEAD' : m < 1 ? 'NECK' :
    m < 1.8 ? '1/2 LENGTH' : rnd(m / 2.4) + ' LENGTHS';
  const dp = replay ? 0 : o[0] === pick ? rnd(stake * (od[pick] - 1)) : -stake;
  pts += dp;
  if (pts < 50) pts = 1000;
  fin = {o: o, mg: mg, dp: dp};
  const n = [], c = [];
  for (const i of o) { n.push(i + 1); c.push(rs.tc[i]); }
  log('FINISH race=' + raceNo + ' seed=' + hex(F.seed) + ' order=' + n + ' t=' + c + ' margin=' + mg);
  if (!replay) { log('RESULT pick=' + (pick + 1) + ' place=' + (o.indexOf(pick) + 1) + ' delta=' + dp + ' points=' + pts); save(); }
  notes = (o[0] === pick ? WIN : LOSE).slice();
}

// The screen's rect, then the frame's draws: the course (with the screen
// and its feed) for camera c, or HEAD ON; then extra ([plan, inputs] or null).
function paint(c, xs, close, gate, extra) {
  // Its face on this camera, or null out of view or over the map
  // (y < 13): fill, bezel, feed and lettering all use these integers.
  const a = pose(VS[0] - VS[2], VS[1]), b = pose(VS[0] + VS[2], VS[1]);
  if (pc) {
    // Panning: the same tests on the four corners (the bottom edge is on
    // the horizon); vr the rect inside the face, vq its edges [x, 1/Z'].
    const p = pj(a), q = pj(b), x0 = p[0] / p[1], x1 = q[0] / q[1];
    vq = [x0, 1 / p[1], x1, 1 / q[1]];
    vr = p[1] > .02 && q[1] > .02 && x0 <= 239 && x1 >= 1 && x1 - x0 >= 8 && x1 - x0 <= 160 && 28 - 10 * mx(vq[1], vq[3]) >= 13 ?
      [rnd(x0), M.ceil(28 - 10 * mn(vq[1], vq[3])), rnd(x1), 28] : null;
  } else {
    const p = c[0] / a[1];
    vr = [rnd(120 + (a[0] - cx) * p), rnd(c[2] + (c[1] - VS[4]) * p), rnd(120 + (b[0] - cx) * p), rnd(c[2] + (c[1] - VS[3]) * p)];
    if (vr[0] > 239 || vr[2] < 1 || vr[1] < 13 || vr[1] > 134 || vr[2] - vr[0] < 8 || vr[2] - vr[0] > 160) vr = null;
  }
  if (scene !== 'race' || cm === 6) vr = null;
  if (vr) ++von;
  H.beginFrame(4);
  // The dust and the band (KN[4]; docs/apps/derby-finish-fx.md): layers 0, 1
  // (dust: far, mid), HEAD ON's horses, 2 (bokeh), 3 (streaks), 4 (band). A
  // layer's slots scroll by its parallax times d, the pan since the cut (cx,
  // px at 16 m) and the drift; t counts from the cut.
  const e = KN[4], q = scene !== 'race' ? -1 : cm === 3 ? 0 : cm === 6 ? 5 : -1;
  if (e[19] !== q) e[17] = t, e[18] = cx, e[19] = q;
  if (q === 5) dr('hd', []);
  else course(c, cx, xs, close, gate);
  for (let j = 0, f = t - e[17], d = e[14] * f - (q ? 0 : (cx - e[18]) * c[0] / 16); j < (q < 0 ? 0 : 5); ++j) {
    // HEAD ON: a still camera 12 m past the line, 2.2 m up, looks back
    // down the course; each horse scaled by its own 1/z (JS divides),
    // the last (farthest) first.
    if (j === 2 && q) for (let i = 7; i >= 0; --i) {
      const l = ro[i], q = 400 / (D + 12 - xs[l]), s = .25 * q * sin(ph[l]);
      dr('fr', [120 + (DL[l] - 16.5) * q, 50 + 2.2 * q, q / 6, F.h[l].coat, SILK[l], mx(0, s), mx(0, -s)]);
    }
    const w = e[q + j];
    if (!w) continue;
    if (j > 3) { dr('band', [w, flo(f / e[16]) % 6]); continue; }
    const o = e[10 + j] * d, k = flo(-o / w) - 1;
    dr(j - 2 ? 'dust' : 'bokeh', [f * e[15], k * w + o, k * 2.39996 % (2 * PI), w, j]);
  }
  if (extra) dr(extra[0], extra[1]);
  H.commit();
}
