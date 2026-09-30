'use strict';
const V = pocket.kasane, H = V.procedural, K = pocket.input.keys, log = m => console.log('DERBY ' + m);
// Frames now, while the heap still has holes that size (MEGADEMO's lesson).
H.beginFrame(0);
const res = H.resource(), live = {}, queue = [];
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
    // The plan n: its code, and the points of a typed-point plan.
    const k = KN[tier], i = +n[1];
    let p;
    if (n[0] === 'g' && i >= 0) {
      // Gallop frame i of the close-up horse.
      const x = [], y = [], ph = PI * i / 3, b = bob(i), put = (u, v) => { x.push(rnd(8 * u)); y.push(rnd(8 * v)); };
      let jx = 0, jy = 0;
      for (let j = 0; j < BODY.length; j += 2) {
        const u = BODY[j], l = BODY[j + 1];
        if (u < 20) { put(jx = u, jy = l + b); continue; }
        const a = .55 * sin(ph + u - 20), kx = jx + l * sin(a), ky = jy + l * M.cos(a), a2 = a - .9 * mx(0, sin(ph + u - 18.6));
        put(kx, ky); put(kx + l * sin(a2), ky + l * M.cos(a2)); put(kx, ky); put(jx, jy);
      }
      p = [prog(T.nil), {kind: 'affineQ14Points', x: x, y: y, color: 0xef5b, coeff: [8192, 0, 0, 8192, HX * 16384, HY * 16384]}];
    } else if (n === 'hd') p = [prog(T.nil), {kind: 'affineQ14Points', x: HD[0], y: HD[1], color: 0xad55, coeff: [16384, 0, 0, 16384, 0, 0]}];
    else if (n[0] === 'r' && i >= 0) p = [prog(T.runner, [F.h[i].coat, SILK[i], SILK[i] ^ 0x8410])];
    else if (n[0] === 't' && i >= 0) p = [prog(T.pt, [512 + 288 * i])];
    else p = [prog(T[n], k.concat(1 / k[2], M.ceil(k[2] / 2)))];
    live[n] = p[1] ? H.register(p[0], p[1]) : H.register(p[0]);
    queue.shift(); ++reg;
  } catch (e) { log('LOADFAIL ' + n + ' ' + e); queue.push(queue.shift()); }
}
// Draws plan n now if it is registered (inputs are copied: pocket_proc.c).
const dr = (n, a) => { if (live[n]) H.draw(live[n], a); };
const want = l => { for (const n of l) if (!live[n] && queue.indexOf(n) < 0) queue.push(n); };
function drop(l) {
  for (const n of l) {
    if (live[n]) H.unregister(live[n]);
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
  try { A.tone({frequencyHz: n[0], durationMs: n[1], gain: .45}).then(done, done); } catch (e) { busy = 0; }
}
// Notes as Hz, ms pairs; WIN and LOSE open with the camera click.
const FANFARE = [523, 110, 659, 110, 784, 110, 1047, 260], BELL = [1568, 60, 1568, 60, 1568, 260],
  WIN = [2637, 25, 784, 90, 988, 90, 1175, 90, 1568, 320], LOSE = [2637, 25, 392, 160, 330, 260];

// ---- State
let pts = 1000, raceNo = 1, pick = 0, stake = 100, scene = '', t = 0, cam = 0, camT = 0, R = [], need = 0;
let rs = null, od = null, fin = null, photoX = null, replay = 0, ph = z8(0);
let cx = 0, disp = 0, slow = 0, ld = -1, cm = 0, hold = 0, dl = 0, man = 0, cl = 0, vr = null, von = 0, ro = [0, 1, 2, 3, 4, 5, 6, 7];
const hex = s => ('0000000' + s.toString(16).toUpperCase()).slice(-8), num = i => 'NO.' + (i + 1),
  th = p => (p + 1) + (['ST', 'ND', 'RD'][p] || 'TH');
// Seeds: the hardware's, mixed with the stored race count (README).
const ST = pocket.storage, HW = pocket.random.seed();
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
// and top height (m), a 4:1 face.
const VS = [840, 34, 20, 6, 16], FN = [3, 4, 6], LO = [3, -25, -30, 1, 1, 4, -30, -2, -26, 12, 12, 8];
// The course: sections [m, curvature 1/m] from the start. pose(g, w): the
// point g m along it, w m out from the side cameras' line (the side view's
// depth: inner rail 11, screen 34, stands 40), as [x, z, tangent]. The
// straight is one section and gives [g, w, 1, 0] exactly.
const CRS = [[2e3, 0]];
function pose(g, w) {
  let x = 0, z = 0, a = 0;
  for (const e of CRS) {
    const s = mn(g, e[0]), b = a + e[1] * s;
    if (e[1]) x += (sin(b) - sin(a)) / e[1], z -= (M.cos(b) - M.cos(a)) / e[1];
    else x += s * M.cos(a), z += s * sin(a);
    a = b; g -= s;
    if (g <= 0) break;
  }
  x += g * M.cos(a); z += g * sin(a);
  return [x - w * sin(a), z + w * M.cos(a), M.cos(a), sin(a)];
}
// ---- Cameras, a row a unit: [f, height, horizon y, lead, smoothed, lo,
// hi]. A side unit looks along the course's normal from cx: the leader
// lead/f m ahead of it (eased when smoothed, set on a cut), cx held within
// lo..hi. WIDE, CLOSE (set per lane), FIELD, FINISH (slow motion), PHOTO
// (along the line: the line is x=120), VISION (low, within 30 m of the
// screen); HEAD ON is a still (paint).
const CAMS = [[100, 9.7, 33, 880, 1, -1e9, 1e9], 0, [58, 15, 36, 880, 1, -1e9, 1e9], [170, 7, 22, 640, 0, -1e9, D - 6],
  [300, 4, 30, 880, 0, D, D], [130, 1.6, 84, 880, 1, VS[0] - 30, VS[0] + 30], 0,
  [200, 6, 28, 100, -14], [200, 6, 28, 460, -14], [200, 6, 28, 820, -14]],
  NAMES = ['WIDE', 'CLOSE', 'FIELD', 'FINISH', '', 'VISION', 'HEAD ON', 'WIDE 1', 'WIDE 2', 'WIDE 3'];
// Panning units (rows 7..9, [least f, height, horizon y, g, w]; README):
// turned to the leader, f keeping it PAN[1] px long, at most PAN[2]. WIDE
// takes the nearest when it is over PAN[0] m from the leader. Height 6 and
// horizon 28 are baked in the pan plans. pc: the unit in use, [x, z, unit
// vector to the aim, f, distance to the aim], null for a side unit.
const PAN = [60, 14, 1500];
let pc = null, vq = null, Lo, Hi, SQ, SU, SV;
function wide(g) {
  let m = 0, e = 1e9;
  for (let i = 7; i < 10; ++i) {
    const q = pose(CAMS[i][3], CAMS[i][4]), a = pose(g, (DNR + DFR) / 2), x = a[0] - q[0], z = a[1] - q[1];
    if (x * x + z * z < e) e = x * x + z * z, m = i;
  }
  return e > PAN[0] * PAN[0] ? m : 0;
}
// Sets cx for shot m (1: locked on lane l) and returns the camera.
function shot(m, xs, l, cut) {
  pc = null;
  if (m === 1) {
    const f = 24 * DL[l];
    cx = xs[l] - 12.5 * U - (HX - 120) * DL[l] / f;
    return [f, 3, HY + 6 * HS - 72];
  }
  const c = CAMS[m];
  if (m > 6) {
    const q = pose(c[3], c[4]), a = pose(mx.apply(null, xs), (DNR + DFR) / 2), x = a[0] - q[0], z = a[1] - q[1], e = M.sqrt(x * x + z * z);
    pc = [q[0], q[1], x / e, z / e, mn(PAN[2], mx(c[0], PAN[1] * e / 2.4)), e];
    return [pc[4], c[1], c[2]];
  }
  const tgt = mx.apply(null, xs) - c[3] / c[0];
  cx = mx(c[5], mn(c[6], c[4] ? cut ? tgt : cx + (tgt - cx) * .12 + mx.apply(null, rs.v) * DT * .88 : tgt));
  return c;
}
// Draws one frame of the course (dr, no list), back to front, for camera c
// at x0. K: the screen's face when this is its feed, a camera 6 m behind
// the leader, low on the rail: rails and the leading FN[tier] runners,
// those wholly inside K.
function course(c, x0, xs, close, gate, K) {
  if (!K && pc) return pan(xs);
  const f = c[0], h = c[1], hy = c[2], k = KN[tier], o = K ? (K[0] + K[2]) / 2 : 120, R = K ? K[2] - 1 : 245,
    sx = (w, d0) => o + (w - x0) * f / d0, gy = d0 => hy + h * f / d0, ty = (d0, e) => hy + (h - e) * f / d0;
  const rail = (d0, col) => {
    const p = f / d0, s = k[4] * p, a = sx((K ? M.ceil : flo)((x0 - (o - (K ? K[0] : -5)) / p) / k[4]) * k[4], d0);
    dr('rail', [a, s, ty(d0, 1.1), gy(d0), mx(0, K ? flo((R - a) / s) : mn(flo((700 - a) / s), M.ceil((R - a) / s))), ty(d0, .55), col]);
  };
  let q = f / 40, n;
  if (!K) {
    // Stands and crowd at 40 m, a pillar every 12 m. The crowd's phase is
    // wrapped and centred on the row: the VM's sin slows 7x past |x| 201.
    const j = flo((x0 - 130 / q) / 12), a = sx(j * 12, 40), dx = 12 * q;
    n = mn(flo((700 - a) / dx), M.ceil((250 - a) / dx) + 1);
    dr('stands', [a, dx, gy(40), -2.4 * q, n, 0, 0, ty(40, 13.5)]);
    dr('crowd', [a, dx, gy(40), -2.4 * q, n, j * k[2] * 2.39996 % (2 * PI) - 2 * PI * rnd(n * k[2] * .191), (t >> 3 ^ t) & 1, t & 1]);
    // The screen in front of the stands: dark, a grey flash, then its feed.
    if (vr) {
      dr('vis', [vr[0] - 1, vr[1] - 1, vr[2], vr[3], mx(1, rnd(f / VS[1] * .4)), gy(VS[1]), von < 8 ? 0 : von < 11 ? 0x632c : 0x0866]);
      feed(xs, (vr[2] - vr[0]) / 120);
    }
  }
  rail(DFR, 0xad55);
  if (!K) {
    // Turf stripes: lines of constant distance, so they meet at the vanishing
    // point; none whose near end is left of -470 (the VM's -480 limit: a
    // close-up of a far lane at LIGHT's 10 m spacing reached -499).
    q = f / DFR;
    const w = k[5], i0 = mx(flo((x0 - 125 / q) / w), M.ceil((x0 - 590 * DNR / f) / w)), xf = sx(i0 * w, DFR), xn = sx(i0 * w, DNR);
    n = mx(0, mn(M.ceil((250 - xf) / (w * q)), flo((700 - xn) / (w * f / DNR))));
    dr('turf', [xn, xf, w * f / DNR, w * q, gy(DNR), gy(DFR), n, i0 & 1]);
    for (let m = 200; m <= D; m += 200) {
      const p = sx(m, DFR), e = m === D;
      if (p > -40 && p < 280)
        dr('pole', [p, gy(DFR), ty(DFR, e ? 4 : 2.6), (e ? .55 : .3) * q, e ? sx(m, DNR) : p, gy(e ? DNR : DFR), p, gy(DFR)]);
    }
    // The gate at 0 m: 9 stall posts, lane boundaries one MUL apart in depth.
    q = f * M.sqrt(Q) / DL[0];
    if (gate && M.abs(x0 * q) < 400) dr('gate', [-x0 * q, h * q, (h - 2.6) * q, 1 / Q, hy]);
  }
  for (let l = 7; l >= 0; --l) {
    if (K && ro.indexOf(l) >= FN[tier]) continue;
    const p = f / DL[l], S = p * U, X = o + (xs[l] - 12.5 * U - x0) * p, Y = gy(DL[l]), a = ph[l];
    if (l === close) {
      const g = (flo(a * 3 / PI) % 6 + 6) % 6;
      dr('g' + g, []);
      dr('silk', [SILK[l], rnd(HS * bob(g))]);
    } else if (K ? X - 2.5 * S >= K[0] && X + 12.5 * S <= R : X > -160 && X < 400) rin(l, X, S, Y, a);
  }
  rail(DNR, 0xffff);
}
// A runner: hip x, back y, px per unit, ground y, four hooves.
const rin = (l, X, S, Y, a) => dr('r' + l, [X, Y - 6 * S + .6 * S * sin(2 * a), S, Y, X + S * (.5 + 3 * sin(a)),
  X + S * (.5 + 3 * sin(a + .8)), X + S * (8 + 3 * sin(a + 3.3)), X + S * (8 + 3 * sin(a + 4.1))]);
// The screen's feed on its face vr at scale z: a low camera 6 m behind the leader.
function feed(xs, z) {
  if (von > 10) course([85 * z, 3, vr[1] + 4.8 * z], mx.apply(null, xs) - 6, xs, -1, 0, vr);
}
