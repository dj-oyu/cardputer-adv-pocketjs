'use strict';
// ---- Scenes: pad (pick), gate (runner plans load one a frame: horses
// enter the stalls), race, photo (still), res (result).
function enter(s) {
  scene = s; t = 0; need = 1;
  if (s === 'pad') { drop(['conf']); want(PAD); replay = 0; F = field((rng(HW ^ M.imul(sn, 0x9e3779b9))() * 4294967296 + M.imul(raceNo, 0x9e3779b9)) >>> 0); log('ODDS ' + (od = odds(F.h))); }
  if (s === 'gate') { drop(['conf']); want(RUN); rs = race(F); ph = [0, 1, 2, 3, 4, 5, 6, 7]; cam = disp = slow = cm = hold = dl = man = von = 0; ld = -1; notes = FANFARE.slice(); }
  if (s === 'race') notes = BELL.slice();
  if (s === 'photo') want(['photo']);
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
    s = [(dm ? '' : 'RACE ' + raceNo + '  ') + '1000M STRAIGHT  SEED ' + hex(F.seed), num(pick) + ' ' + h.n,
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
// and its feed) for camera c, or HEAD ON; then extra (map, photo, conf).
function paint(c, xs, close, gate, extra) {
  // Its face on this camera, or null out of view or over the map
  // (y < 13): fill, bezel, feed and lettering all use these integers.
  const a = pose(VS[0] - VS[2], VS[1]), b = pose(VS[0] + VS[2], VS[1]);
  if (pc[2]) {
    // Panning: the same tests on the four corners; vr the rect inside the
    // face, vq its edges [x, 1/Z~] (pj).
    const p = pj(a[0], a[1]), q = pj(b[0], b[1]), x0 = p[0] / p[1], x1 = q[0] / q[1], h = pc[6], y = pc[7],
      t = y + (1 - VS[4] / h) * mx(1 / p[1], 1 / q[1]), u = y + (1 - VS[4] / h) * mn(1 / p[1], 1 / q[1]),
      w = y + (1 - VS[3] / h) * mx(1 / p[1], 1 / q[1]);
    vq = [x0, 1 / p[1], x1, 1 / q[1]];
    vr = p[1] > 0 && q[1] > 0 && x0 <= 239 && x1 >= 1 && x1 - x0 >= 8 && x1 - x0 <= 160 && t >= 13 && t <= 134 ?
      [rnd(x0), M.ceil(u), rnd(x1), M.floor(y + (1 - VS[3] / h) * mn(1 / p[1], 1 / q[1]))] : null;
    if (vr && w < vr[3]) vr = null;
  } else {
    const p = c[0] / a[1];
    vr = [rnd(120 + (a[0] - cx) * p), rnd(c[2] + (c[1] - VS[4]) * p), rnd(120 + (b[0] - cx) * p), rnd(c[2] + (c[1] - VS[3]) * p)];
    if (vr[0] > 239 || vr[2] < 1 || vr[1] < 13 || vr[1] > 134 || vr[2] - vr[0] < 8 || vr[2] - vr[0] > 160) vr = null;
  }
  if (scene !== 'race' || cm === 6) vr = null;
  if (vr) ++von;
  let d = [['hd', []]];
  if (cm === 6 && scene === 'race') {
    // HEAD ON: a still camera 12 m past the line, 2.2 m up, looks back
    // down the course; each horse scaled by its own 1/z (JS divides),
    // the last (farthest) first.
    for (let i = 7; i >= 0; --i) {
      const l = ro[i], q = 400 / (D + 12 - xs[l]), s = .25 * q * sin(ph[l]);
      d.push(['fr', [120 + (DL[l] - 16.5) * q, 50 + 2.2 * q, q / 6, F.h[l].coat, SILK[l], mx(0, s), mx(0, -s)]]);
    }
  } else d = course(xs, close, gate);
  H.beginFrame(4);
  for (const e of d.concat(extra)) if (live[e[0]]) H.draw(live[e[0]], e[1]);
  H.commit();
}
// ---- One projection for every camera (README "Panning units"): a world
// point in pc's view over its height, [X~, Z~]: screen x = X~/Z~, the
// ground at hy + 1/Z~, a point e m up at hy + (1 - e/h)/Z~. A side unit is
// the case of a view along the normal (every point of a line at one depth).
function pj(x, z) {
  const a = x - pc[0], b = z - pc[1], Z = (a * pc[2] + b * pc[3]) / pc[4];
  return [(a * pc[3] - b * pc[2] + pc[8] * Z) / pc[6], Z / pc[6]];
}
// Narrows Lo..Hi to c0 + c1 g >= 0.
function lim(c0, c1) { if (c1 > 0) Lo = mx(Lo, -c0 / c1); else if (c1 < 0) Hi = mn(Hi, -c0 / c1); else if (c0 < 0) Hi = -1e9; }
// Plan n along the straight at depth w, a point every s m on g0's grid, in
// view (x -40..280, depth to zf, y in the VM's range) and one point past
// each end, or inside the window WX (the feed); far to near: the VM finds
// each 1/Z~ by Newton from the last one, which is farther, so it rises to
// it and cannot diverge (the first from 1, 20 steps: 1/Z~ from .5 to 4000).
// Strides double toward the far end where points close to L px, but never
// a step of more than .3 of the depth (3 steps: .3^8), the halves on the
// grid. Inputs [X~, dX~, -Z~, -dZ~, count, a, b, horizon]; a null a is the
// crowd's phase, from the first bay's index (a bay's dots jitter the same
// in every frame). prail's lines reach the point after the last.
function ser(d, n, w, s, g0, L, zf, a, b) {
  const q = SQ = pj(g0, w), u = SU = pc[2] / pc[4] / pc[6], v = SV = (pc[3] + pc[8] * pc[2] / pc[4]) / pc[6],
    x0 = WX ? WX[0] : -40, x1 = WX ? WX[2] - 1 : 280;
  Lo = -1e9; Hi = 1e9;
  lim(q[1] - ZM, u); lim(zf / pc[4] / pc[6] - q[1], -u); lim(q[0] - x0 * q[1], v - x0 * u); lim(x1 * q[1] - q[0], x1 * u - v);
  if (!(Lo < Hi)) return;
  const o = u < 0 ? -1 : 1, U = u * o, V = v * o, t1 = o > 0 ? Hi : -Lo, t0 = o > 0 ? Lo : -Hi;
  let k = s, Z = M.sqrt(M.abs(v * q[1] - q[0] * u) * s / L), n0 = WX ? M.ceil(t0 / s) * s : flo(t0 / s) * s, t;
  while (Z < q[1] + t1 * U && 2 * k * U < .3 * (q[1] + t1 * U) && k < 64 * s) k *= 2, Z *= M.SQRT2;
  t = WX ? flo(t1 / k) * k : M.ceil(t1 / k) * k;
  if (!WX && !inr(q, U, V, t)) t -= k;
  if (!WX && !inr(q, U, V, n0)) n0 += s;
  for (;;) {
    Z /= M.SQRT2;
    const h = k > s ? M.ceil(mx(U > 1e-7 ? (mx(Z, k * U / .3) - q[1]) / U : -1e9, n0) / k) * k : n0, c = rnd((t - h) / k), z = q[1] + t * U,
      j = o * (g0 + o * t) / s;
    if (c > 0 && z < 1.6)
      d.push([n, [q[0] + t * V, -V * k, -z, U * k, mn(255, c), a === null ? -j * 2.39996 % (2 * PI) - 2 * PI * rnd(c * .191) : a, b, pc[7]]]);
    if (k === s) return;
    t = mn(t, h); k /= 2;
  }
}
function inr(q, U, V, t) {
  const z = q[1] + t * U, x = (q[0] + t * V) / z;
  return z > ZM && x > -400 && x < 640;
}
// Lines across the last ser()'s view (or between the ends e = [x, 1/Z~, x,
// 1/Z~]): coefficient c0 (1 - height/h), then cs a line, m of them, the two
// ends joined.
function hl(d, c0, cs, m, col, e) {
  if (!e) {
    const a = SQ[1] + Lo * SU, b = SQ[1] + Hi * SU;
    if (!(Lo < Hi)) return;
    e = [(SQ[0] + Lo * SV) / a, 1 / a, (SQ[0] + Hi * SV) / b, 1 / b];
  }
  d.push(['hl', [e[0], pc[7] + c0 * e[1], cs * e[1], e[2], pc[7] + c0 * e[3], cs * e[3], m, col]]);
}
// One frame from pc: stands (pillars, roof, tiers), crowd, the screen, far
// rail, turf, poles, the gate (side units), runners, near rail; inside the
// window WX (the screen's feed): the rails and the leading FN[tier]
// runners. Nothing deeper than 150 m past the leader (75 at HEAVY); posts
// 4 px apart at least (8 with the screen in a panning view).
function course(xs, close, gate) {
  const k = KN[tier], d = [], h = pc[6], hy = pc[7], f = pc[4], zf = pc[5] + (tier > 1 ? 75 : 150), p = pc[2] !== 0,
    L = vr && p ? 8 : 4, K = WX, R = K ? K[2] - 1 : 400, q = 11.6 * pc[3] / f / h;
  ZM = 1 / mn(650 - hy, (450 + hy) * h / 13.5);
  if (!K) {
    ser(d, 'prail', 40, 12, 0, L, zf, 31727, 1 - 13.5 / h);
    hl(d, 1, -2.4 / h, k[0], 21130);
    ser(d, 'c' + ((t >> 3) & 1), 40, 12, 0, 0, zf, null, -2.4 / h);
    if (vr && p) {
      const c0 = 1 - VS[3] / h, c1 = 1 - VS[4] / h, m = M.ceil((c0 - c1) * mx(vq[1], vq[3])) + 1;
      hl(d, c0, (c1 - c0) / (m - 1), m, von < 8 ? 0 : von < 11 ? 0x632c : 0x0866, vq);
      hl(d, c0, c1 - c0, 2, 10565, vq);
      feed(d, xs, mn((vr[2] - vr[0]) / 120, (vr[3] - vr[1] - 1) / 30));
    } else if (vr) {
      d.push(['vis', [vr[0] - 1, vr[1] - 1, vr[2], vr[3], mx(1, rnd(f / VS[1] * .4)), hy + h * f / VS[1], von < 8 ? 0 : von < 11 ? 0x632c : 0x0866]]);
      feed(d, xs, (vr[2] - vr[0]) / 120);
    }
  }
  ser(d, 'prail', DFR, k[4], 0, L, zf, 0xad55, 1 - 1.1 / h);
  if (!K) {
    ser(d, 't0', DNR, 2 * k[5], 0, L, zf, pc[8] * q - 11.6 * pc[2] / h, -q);
    ser(d, 't1', DNR, 2 * k[5], k[5], L, zf, pc[8] * q - 11.6 * pc[2] / h, -q);
    for (let m = 200; m <= D; m += 200) {
      const a = pj(m, DFR), r = 1 / a[1], x = a[0] * r, e = m === D, n = e ? pj(m, DNR) : a, y = n[0] / n[1];
      if (a[1] > ZM && n[1] > ZM && x > -40 && x < 280 && y > -400 && y < 640)
        d.push(['pole', [x, hy + r, hy + (1 - (e ? 4 : 2.6) / h) * r, (e ? .55 : .3) * r / h, y, hy + 1 / n[1], x, hy + r]]);
    }
    // The gate at 0 m (side units): 9 stall posts, lane boundaries one MUL apart in depth.
    const g = f * M.sqrt(Q) / DL[0];
    if (gate && !p && M.abs(pc[0] * g) < 400) d.push(['gate', [-pc[0] * g, h * g, (h - 2.6) * g, 1 / Q, hy]]);
  }
  for (let l = 7; l >= 0; --l) {
    if (K && ro.indexOf(l) >= FN[tier]) continue;
    const a = pj(xs[l] - 12.5 * U, DL[l]), r = 1 / a[1], X = a[0] * r, S = r / h * U;
    if (l === close) {
      const g = (flo(ph[l] * 3 / PI) % 6 + 6) % 6;
      d.push(['g' + g, []], ['silk', [SILK[l], rnd(HS * bob(g))]]);
    } else if (a[1] > ZM && (K ? X - 2.5 * S >= K[0] && X + 12.5 * S <= R : X > -160 && X < 400))
      d.push(rin(l, X, S, hy + r, ph[l]));
  }
  ser(d, 'prail', DNR, k[4], 0, L, zf, 0xffff, 1 - 1.1 / h);
  return d;
}
