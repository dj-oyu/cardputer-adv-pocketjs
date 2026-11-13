'use strict';
// ---- Scenes: pad (pick), gate (runner plans load one a frame: horses
// enter the stalls), race, photo (still), res (result).
function enter(s) {
  scene = s; t = 0; need = 1;
  if (s === 'pad') { drop(['conf', 'band']); want(PAD); replay = 0; F = field(add(rng(HW ^ M.imul(sn, 0x9e3779b9))(), M.imul(raceNo, 0x9e3779b9))); log('ODDS ' + (od = odds(F.h, F.o)).map(ot) + (F.o ? ' OVAL' : ''));
    // The oval's bend in 16 chords (pocket.derby's series).
    CRS = F.o ? OC : 0; CH = [-655360000];
    if (F.o) for (let j = 0; j <= 16; ++j) CH.push(OB + j * 1544156);
    CH.push(655360000); S.course(CH, CRS, 720896);
  }
  if (s === 'gate') { drop(['conf']); want(RUN); fin = photoX = null; rs = race(F); rs.l = lz.map((_, i) => PW[20 * i]); F.k = z8(0);
    for (let i = 0; i < 64; ++i) F.k[i >> 3] += F.m[i & 7] > F.m[i >> 3]; cam = disp = slow = cm = hold = dl = man = von = 0; ld = -1; notes = FANFARE.slice(); }
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
        tt([8 + 30 * i, 13, 32 + 30 * i, 24, 0xd8e8ffff, 4], od[i] < 100 ? ot(od[i], 1) : '' + idiv(od[i] + 5, 10))));
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
      STY[h.sty] + ' x' + ot(od[pick]), 'BET ' + stake + '  PTS ' + pts + '   A/D HORSE E/S BET 1 GO'];

    if (R.pick !== pick) {
      R.sel.setRect(tx, [1 + 30 * pick, 13, 31 + 30 * pick, 24]);
      // The figures' fractions (Q16): where each lies in its range.
      const v = [h.qt, h.qs, h.qk];
      for (let i = 0; i < 3; ++i) R.bar[i].setRect(tx, [180, 43 + 12 * i, 182 + (52 * v[i] + 32768 >> 16), 49 + 12 * i]);
      R.pick = pick;
    }
  } else if (scene === 'res') {
    if (R.t[3].s !== undefined) return;
    const o = fin.o, p = o.indexOf(pick);
    s = [(replay ? 'REPLAY  ' : 'WINNER  ') + num(o[0]) + ' ' + F.h[o[0]].n + '  ' + fin.mg,
      '1ST ' + (o[0] + 1) + '  2ND ' + (o[1] + 1) + '  3RD ' + (o[2] + 1) + '  4TH ' + (o[3] + 1),
      'YOUR ' + num(pick) + ' ' + th(p) + (replay ? '' : '  ' + (fin.dp < 0 ? '' : '+') + fin.dp),
      'PTS ' + pts + '   1 NEXT RACE   R REPLAY'];
  } else {
    const o = scene === 'photo' ? fin.o : ro, lead = rs ? mx.apply(null, XQ) : 0, ph2 = scene === 'photo';
    s = [(o[0] + 1) + '-' + (o[1] + 1) + '-' + (o[2] + 1) + '   ' + num(pick) + ' ' + th(o.indexOf(pick)),
      scene === 'gate' ? 'GATE' : mx(0, 65568768 - lead >> 16) + 'M', camT > 0 ? NAMES[cm] : '',
      ph2 ? (t < 50 ? (t & 8 ? 'PHOTO' : '') : num(fin.o[0])) : '', ph2 && t >= 50 ? fin.mg : ''];
  }
  for (let i = 0; i < s.length; ++i) if (R.t[i].s !== s[i]) R.t[i].setText(tx, R.t[i].s = s[i]);
}
// The runners u (Q12) of the way through the last sim step (Q16 m; XQ the
// step's own); their lanes (README "レーンの横位置"): once a step the oval
// steers w at .8 m/s at most, lz sways (sinMany). VM: the leader's speed .88
// of a step (Q16 m), for the camera.
const XQ = new Int32Array(8);
let VM = 0;
function at(u) {
  const w = rs.l, k = F.k, n = rs.t > (w.t || 0);
  w.t = rs.t;
  for (let i = 0; i < 8; ++i) { const p = rs.px[i]; XQ[i] = rs.x[i]; XS[i] = p + ((XQ[i] - p) * u >> 12); }
  VM = (mx.apply(null, rs.v) >> 8) * 57672 >> 12;
  const L = mx.apply(null, XS);
  for (let i = 0, x, s; i < 8; ++i) {
    x = XQ[i]; s = F.h[i].sty;
    if (n && F.o) w[i] += mx(-2621, mn(2621, PW[x < 17891328 ? i * (6 + 7 * s) : x < 42598400 ? 20 * k[i] : mn(140, 20 * (k[i] + s))] - w[i]));
    LT[i] = ((XS[i] >> 6) * 261 >> 10) + 25033 * i;
  }
  SK.sinMany(LT, LS, 0);
  // imul: 0 times a negative sine is -0, a double, with '*'.
  for (let i = 0; i < 8; ++i) lz[i] = w[i] + (M.imul(mn(26214, (L - XS[i] >> 4) * 1966 >> 12), LS[i]) >> 16);
  zo.sort((a, b) => lz[b] - lz[a]);
  return XS;
}
// Keep all projection inputs from the winner's crossing, before at() moves them.
function photo(u) {
  if (u !== undefined) {
    const x = at(u).slice();
    x.z = lz.slice(); x.o = zo.slice();
    return x;
  }
  lz = photoX.z; zo = photoX.o;
  return photoX;
}
// Finish: run the rest of the field unseen, then settle the bet.
function settle() {
  while (rs.done < 8 && rs.t < 262144000) step(F, rs);
  // The margin (Q16 m): the second's time behind (Q16 steps) at its speed.
  const o = order(rs), a = rs.tc[o[1]] - rs.tc[o[0]], m = (a >> 6) * (rs.v[o[1]] >> 10) >> 4;
  const mg = m < 1311 ? 'DEAD HEAT' : m < 7864 ? 'NOSE' : m < 19661 ? 'SHORT HEAD' : m < 39322 ? 'HEAD' : m < 65536 ? 'NECK' :
    m < 117965 ? '1/2 LENGTH' : idiv(m + 78643, 157286) + ' LENGTHS';
  const dp = replay ? 0 : o[0] === pick ? idiv(stake * (od[pick] - 10) + 5, 10) : -stake;
  pts += dp;
  if (pts < 50) pts = 1000;
  fin = {o: o, mg: mg, dp: dp};
  const n = [], c = [];
  for (const i of o) { n.push(i + 1); c.push((rs.tc[i] >> 6) * 50 >> 10); }
  log('FINISH race=' + raceNo + ' seed=' + hex(F.seed) + ' order=' + n + ' ms=' + c + ' margin=' + mg);
  if (!replay) { log('RESULT pick=' + (pick + 1) + ' place=' + (o.indexOf(pick) + 1) + ' delta=' + dp + ' points=' + pts); save(); }
  notes = (o[0] === pick ? WIN : LOSE).slice();
}

// The screen's rect, then the frame's draws: the course (with the screen
// and its feed) for camera c, or HEAD ON; then extra ([plan, inputs] or null).
const VR = new Int32Array(4), HO = new Int32Array(24), RW = {14: 4681, 20: 3277, 60: 1092};
function paint(c, xs, close, gate, extra) {
  // Its face on this camera (pocket.derby.screen), or null out of view or
  // over the map (y < 13): fill, bezel, feed and lettering use these px.
  if (scene === 'race' && cm !== 6) {
    if (!pc) S.side(c[0], c[1], c[2], cx, 0, 0);
    vr = S.screen(VR, pc ? 1 : 0, VS[0], VS[1], VS[2], VS[3], VS[4]) ? VR : null;
  } else vr = null;
  if (vr) ++von;
  SK.beginFrame(4);
  // The dust and the band (KN[4]; docs/apps/derby-finish-fx.md): 0 the dust,
  // HEAD ON's horses, 1 the bokeh, 2 the band. A row of slots scrolls by its
  // parallax times d (Q16 px), the pan since the cut (cx, px at 16 m) and the
  // drift, within the camera's area; t counts from the cut.
  const e = KN[4], q = scene !== 'race' ? -1 : cm === 3 ? 0 : cm === 6 ? 7 : -1;
  if (e[22] !== q) e[20] = t, e[21] = cx, e[22] = q;
  if (q === 7) dr('hd', []);
  else course(c, cx, xs, close, gate);
  for (let j = 0, f = t - e[20], d = e[17] * f - (q ? 0 : (cx - e[21] >> 4) * (c[0] >> 16)), r = q ? 0 : e[14] * f; j < (q < 0 ? 0 : 3); ++j) {
    // HEAD ON: a still camera 12 m past the line, 2.2 m up, looks back
    // down the course; each horse scaled by its own 1/z (pocket.derby.head),
    // the last (farthest) first.
    if (j === 1 && q && live.frl && live.frb) {
      bs = 6;
      S.head(xs, lz, HO);
      for (let i = 7; i >= 0; --i) {
        const l = ro[i], o = 3 * l, d = DO(xs[l], l), c = F.h[l].coat;
        bput(live.frl, HO[o], HO[o + 1], HO[o + 2], d, c, SILK[l]);
        bput(live.frb, HO[o], HO[o + 1], HO[o + 2], d, c, SILK[l]);
      }
      bflush();
    }
    const w = e[q + 4 + j];
    if (!w) continue;
    if (j > 1) { dr('band', w, (f * 21846 >> 16) % 6); continue; }
    // The first slot k w + o px from the area's left, -2w..-w; its angle k GA
    // as Q16 turns; t as frames (the plans scale them); px rounded.
    const o = e[15 + j] * d, k = ((0 - o >> 8) * RW[w] >> 24) - 1;
    dr(j ? 'bokeh' : 'dust', f, e[q] + k * w + (o + 32768 >> 16), k * 25032 & 65535, w, e[q + 1], e[q + 2] - (r + 32768 >> 16), e[q + 3] + (r + 32768 >> 16));
  }
  if (extra) dr(extra[0], extra[1]);
  SK.commit();
}

