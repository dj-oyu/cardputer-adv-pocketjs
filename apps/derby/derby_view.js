'use strict';
const V = pocket.kasane, H = V.procedural, SK = V.shiki, K = pocket.input.keys, log = m => console.log('DERBY ' + m);
const SKN = {run: 1, legs: 1, frl: 1, frb: 1}; // Shiki (README)
// Integers only (docs/apps/derby-integer.md): lengths are Q16 m, px Q16
// where a fraction counts, angles Q16 turns. pocket.derby does the divisions.
const S = pocket.derby, QM = 65536;
// Frames now, while the heap still has holes that size (MEGADEMO's lesson).
SK.beginFrame(0);
const res = SK.resource(), live = {}, queue = [];
derby.L = live; // the host oracle names plans by these handles
let reg = 0;
// One plan a frame from frame 1 (info() is null while evaluating), only
// with 18 KB free: a register turn dipped the heap 12.6 KB at most on the
// device (README), leaving 5 KB.
function load() {
  const n = queue[0];
  if (!n) return;
  try {
    if (pocket.memory && pocket.memory.info().internalFreeBytes < 18432) return;
    // The plan n: a built-in plan's name (derby_prog.js), exactly its
    // declared arguments, and the points of a typed-point plan.
    const k = KN[tier], i = +n[1];
    let p;
    if (n[0] === 'g' && i >= 0) {
      // Gallop frame i of the close-up horse: units Q8, BODY in hundredths,
      // a leg's phase o (u - 20 rad) and the frame's i/6 turn in Q16 turns.
      const x = [], y = [], ph = 10923 * i, b = bob(i), put = (u, v) => { x.push(u + 16 >> 5); y.push(v + 16 >> 5); };
      let jx = 0, jy = 0;
      for (let j = 0; j < BODY.length; j += 2) {
        const u = BODY[j], l = BODY[j + 1] * 655 >> 8;
        if (u < 2000) { put(jx = u * 655 >> 8, jy = l + b); continue; }
        const I = M.imul, o = ph + ((u - 2000) * 6675 >> 6), a = I(SK.sin(o), 5737) >> 16, kx = jx + (I(l, SK.sin(a)) >> 16),
          ky = jy + (I(l, SK.cos(a)) >> 16), a2 = a - (mx(0, SK.sin(o + 14603)) * 9387 >> 16);
        put(kx, ky); put(kx + (I(l, SK.sin(a2)) >> 16), ky + (I(l, SK.cos(a2)) >> 16)); put(kx, ky); put(jx, jy);
      }
      p = ['nil', [], {kind: 'affineQ14Points', x: x, y: y, color: 0xef5b, coeff: [8192, 0, 0, 8192, HX * 16384, HY * 16384]}];
    } else if (n === 'hd') p = ['nil', [], {kind: 'affineQ14Points', x: HD[0], y: HD[1], color: 0xad55, coeff: [16384, 0, 0, 16384, 0, 0]}];
    else if (n[0] === 't' && i >= 0) p = ['pt', [512 + 288 * i]];
    else if (n === 'crowd') p = [n, [k[1], k[6], k[7]].concat(KN[3].slice(0, 5))];
    else p = [n, k.slice(0, n === 'stands' ? 4 : 0)];
    live[n] = SKN[n] ? SK.register('derby.' + n) : H.register('derby.' + p[0], p[1], p[2]);
    queue.shift(); ++reg;
  } catch (e) { log('LOADFAIL ' + n + ' ' + e); queue.push(queue.shift()); }
}
// Draws plan n now if it is registered (inputs are copied: pocket_proc.c).
const dr = (n, a, b, c, d, e, f, g, h) => { if (live[n]) H.draw(live[n], a, b, c, d, e, f, g, h); };
const want = l => { for (const n of l) if (!live[n] && queue.indexOf(n) < 0) queue.push(n); };
function drop(l) {
  for (const n of l) {
    if (live[n]) (SKN[n] ? SK : H).unregister(live[n]);
    delete live[n];
    if (queue.indexOf(n) >= 0) queue.splice(queue.indexOf(n), 1);
  }
}

// ---- Sound: audio.tone plays one note at a time; a short note queue.
let A = null, notes = [], busy = 0;
try { if (pocket.capabilities.get('audio.tone').available) A = pocket.audio; } catch (e) {}
const done = () => { busy = 0; }, nop = Boolean; // nop: any function without effects
function sound() {
  if (!A || busy || !notes.length) return;
  const n = notes.splice(0, 2);
  busy = 1;
  // audio.tone's gain is a fraction (a listed boundary).
  try { A.tone({frequencyHz: n[0], durationMs: n[1], gain: .45}).then(done, done); } catch (e) { busy = 0; } // int-lint: allow
}
// Notes as Hz, ms pairs; WIN and LOSE open with the camera click.
const FANFARE = [523, 110, 659, 110, 784, 110, 1047, 260], BELL = [1568, 60, 1568, 60, 1568, 260],
  WIN = [2637, 25, 784, 90, 988, 90, 1175, 90, 1568, 320], LOSE = [2637, 25, 392, 160, 330, 260];

// ---- State
let pts = 1000, raceNo = 1, pick = 0, stake = 100, scene = '', t = 0, cam = 0, camT = 0, R = [], need = 0;
let rs = null, od = null, fin = null, photoX = null, replay = 0;
let cx = 0, disp = 0, slow = 0, ld = -1, cm = 0, hold = 0, dl = 0, man = 0, cl = 0, vr = null, von = 0, ro = [0, 1, 2, 3, 4, 5, 6, 7];
// Lane depths 12 Q^(n/20) m (Q16) for n 0..140: the lanes are PW[20 j];
// at() steers to PW[...] (README "レーンの横位置").
const PW = new Int32Array(141);
PW[0] = 786432;
for (let n = 0; n < 140; ++n) PW[n + 1] = PW[n] + (PW[n] * 268 >> 16);
// Lanes drawn, the order far to near; xs (Q16 m along) and lz (Q16 m deep)
// as Int32Array for pocket.derby; at()'s sway angles a / 40 + 2.4 i (Q16
// turns), one sinMany a frame.
const XS = new Int32Array(8), LT = new Int32Array(8), LS = new Int32Array(8), PO = new Int32Array(32);
let lz = new Int32Array(8).map((_, i) => PW[20 * i]), zo = [7, 6, 5, 4, 3, 2, 1, 0];
const h4 = v => ('000' + v.toString(16).toUpperCase()).slice(-4), hex = s => h4(s >>> 16) + h4(s & 65535), num = i => 'NO.' + (i + 1),
  th = p => (p + 1) + (['ST', 'ND', 'RD'][p] || 'TH');
// Seeds: the hardware's, mixed with the stored race count (README).
const ST = pocket.storage, HW = pocket.random.seed() | 0; // int-lint: allow
let sn = 0;
function save() { if (dm) return; try { ST.set('derby.v1', {v: 1, pts: pts, race: raceNo}).then(nop, nop); } catch (e) {} }
try {
  ST.get('derby.v1').then(r => {
    const v = r && r.value;
    if (v && v.v === 1 && v.pts > 0 && v.race > 0 && scene === 'pad') {
      pts = mx(50, v.pts | 0); raceNo = sn = v.race | 0; enter('pad');
      log('LOADED points=' + pts + ' race=' + raceNo);
    }
  }, nop);
} catch (e) {}

// The screen (README "Turf vision"): centre x, depth, half width, bottom
// and top height (m), a 4:1 face; LO: its lettering's offsets from the face
// (px); FN: the feed's runners.
const VS = [840, 34, 20, 6, 16], FN = [3, 4, 6], LO = [3, -25, -30, 1, 1, 4, -30, -2, -26, 12, 12, 8];
// The course: chord ends (CH, Q16 m) and sections [length, radius (0: a
// straight), x, z, tangent x, z, heading (Q16 turns)], Q16; pose() is
// pocket.derby's. The oval (README "楕円"): the back straight, a 120 m bend
// to the right, the home straight on the straight's line from 650 m. OB: the
// bend's start (650 - 120 pi m).
const OB = 17891910, OC = [[OB, 0, 60490310, -14286848, -QM, 0], [24706490, -7864320, 42598400, -14286848, -QM, 0, 32768],
  [131072000, 0, 42598400, 0, QM, 0]];
let CRS = 0, CH;
// ---- Cameras, a row a unit: [f (Q16 px), height (Q16 m), horizon y (Q16
// px), lead/f (Q16 m: the leader that far ahead of the centre), smoothed,
// lo, hi (Q16 m)]. A side unit looks along the course's normal from cx: the
// leader lead/f m ahead of it (eased when smoothed, set on a cut), cx held
// within lo..hi. WIDE, CLOSE (set per lane), FIELD, FINISH (slow motion),
// PHOTO (along the line: the line is x=120), VISION (low, within 30 m of the
// screen); HEAD ON is a still (paint).
const CAMS = [[6553600, 635699, 2162688, 576717, 1, -2000000000, 2000000000], 0, [3801088, 983040, 2359296, 994339, 1, -2000000000, 2000000000],
  [11141120, 458752, 1441792, 246724, 0, -2000000000, 65142784], [19660800, 262144, 1966080, 192239, 0, 65536000, 65536000],
  [8519680, 104858, 5505024, 443628, 1, (VS[0] - 30) * QM, (VS[0] + 30) * QM], 0,
  [200, 6, 28, 6553600, -917504, -917504], [200, 6, 28, 30146560, -917504, -196608], [200, 6, 28, 53739520, -917504, -917504]],
  NAMES = ['WIDE', 'CLOSE', 'FIELD', 'FINISH', '', 'VISION', 'HEAD ON', 'WIDE 1', 'WIDE 2', 'WIDE 3'];
// Panning units (rows 7..9, [least f (px), height, horizon y, g, w, w on the
// oval: WIDE 2 near its bend's centre (Q16 m)]; README): turned to the
// leader, f keeping it 14 px long, at most 1500. WIDE takes the nearest when
// it is over 60 m from the leader, or the leader is on the oval's bend (the
// side view does not bend: README). Height 6 and horizon 28 are baked in the
// pan plans. PAN: the distance (m), the leader's px, the f cap. PU: the
// units' [g, w] for pocket.derby.wide. pc: the unit in use (null for a side
// unit), its f px at 4.
const PAN = [60, 14, 1500];
let pc = null;
const PU = new Int32Array(6);
function wide(g) {
  for (let i = 0; i < 3; ++i) PU[2 * i] = CAMS[7 + i][3], PU[2 * i + 1] = CAMS[7 + i][CRS ? 5 : 4];
  const r = S.wide(g, PU, PAN[0]);
  return r > 3 || CRS && g > OB && g < 42598400 ? 7 + (r & 3) : 0;
}
// x a / b for 0 <= x, a < 2^15 (Q16 fractions): the high and low halves.
const fm = (x, a) => (x >> 16) * a + ((x & 65535) * a >> 16);
// Sets cx for shot m (1: locked on lane l) and returns the camera [f, h, hy].
function shot(m, xs, l, cut) {
  pc = null;
  if (m === 1) {
    cx = xs[l] - 136533 + 76459;
    return [24 * lz[l], 196608, 2621440];
  }
  const c = CAMS[m];
  if (m > 6) {
    pc = [0, 0, 0, 0, S.aim(c[3], c[CRS ? 5 : 4], mx.apply(null, xs), c[0], PAN[1], PAN[2])];
    return [pc[4] * QM, c[1] * QM, c[2] * QM];
  }
  const tgt = mx.apply(null, xs) - c[3];
  cx = mx(c[5], mn(c[6], c[4] ? cut ? tgt : cx + fm(tgt - cx, 7864) + VM : tgt));
  return c;
}
// Draws one frame of the course (dr, no list), back to front, for camera c
// at x0 (pocket.derby: stands, crowd, rails, turf, poles, gate). K: the
// screen's face when this is its feed, a camera 6 m behind the leader, low on
// the rail: rails and the leading FN[tier] runners, those wholly inside K.
function course(c, x0, xs, close, gate, K) {
  if (!K && pc) return pan(xs);
  const k = KN[tier];
  S.side(c[0], c[1], c[2], x0, K ? K[0] : 0, K ? K[2] : 0);
  if (!K) {
    S.part(0, live.stands, live.crowd || 0, 163840);
    // The screen in front of the stands: dark, a grey flash, then its feed.
    if (vr) {
      S.part(5, live.vis, vr[0], vr[1], vr[2], vr[3], von < 8 ? 0 : von < 11 ? 0x632c : 0x0866, VS[1]);
      feed(xs, (vr[2] - vr[0]) * 546);
      S.side(c[0], c[1], c[2], x0, 0, 0);
    }
  }
  S.part(1, live.rail, 1481114, 0xad55, k[4]);
  if (!K) {
    S.part(2, live.turf, k[5]);
    S.part(3, live.pole);
    if (gate) S.part(4, live.gate);
  }
  S.lanes(xs, lz, PO, 0);
  const L = K ? K[0] * QM : -160 * QM, R = K ? (K[2] - 1) * QM : 400 * QM;
  for (const l of zo) {
    if (K && ro.indexOf(l) >= FN[tier]) continue;
    const X = PO[4 * l], s = PO[4 * l + 1];
    if (l === close) {
      bflush();
      const g = ((DO(xs[l], l) >> 12) * 60495 >> 12) % 6;
      dr('g' + g, []);
      dr('silk', SILK[l], HS * bob(g) + 128 >> 8);
    } else if (K ? X - (5 * s >> 1) >= L && X + (25 * s >> 1) <= R : X > L && X < R) rin(l, X, s, PO[4 * l + 2], DO(xs[l], l));
  }
  bflush();
  S.part(1, live.rail, 720896, 0xffff, k[4]);
}
// Shiki draws batched into one drawMany, Q16 inputs; DO: the legs' phase as
// a distance, xs plus the lane's offset (its gate phase; README).
const BH = new Uint32Array(32), BI = new Int32Array(32 * 7), DO = (x, l) => x + l * 67797;
let bn = 0, bs = 7;
function bput(h, a, b, c, d, e, f, g) { const o = bn * bs; BH[bn++] = h; BI[o] = a; BI[o + 1] = b; BI[o + 2] = c; BI[o + 3] = d; BI[o + 4] = e; BI[o + 5] = f; if (bs > 6) BI[o + 6] = g; }
function bflush() { if (bn) SK.drawMany(BH.subarray(0, bn), BI.subarray(0, bn * bs), bs); bn = 0; }
// A runner: hip X, ground Y, S px per unit (Q16), distance d (Q16 m).
function rin(l, X, S, Y, d) {
  if (!live.run || !live.legs) return;
  bs = 7; const c = F.h[l].coat;
  bput(live.legs, X, S, Y, d, c, 0, 0);
  bput(live.run, X, S, Y, d, c, SILK[l], SILK[l] ^ 0x8410);
}
// The screen's feed on its face vr at scale z (Q16): a low camera 6 m behind
// the leader.
function feed(xs, z) {
  if (von > 10) course([85 * z, 3 * QM, vr[1] * QM + (z << 2) + (z * 13107 >> 14)], mx.apply(null, xs) - 393216, xs, -1, 0, vr);
}
