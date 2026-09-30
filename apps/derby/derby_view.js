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
    const p = spec(n);
    live[n] = p[1] ? H.register(p[0], p[1]) : H.register(p[0]);
    queue.shift(); ++reg;
  } catch (e) { log('LOADFAIL ' + n + ' ' + e); queue.push(queue.shift()); }
}
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
// Panning units (rows 7..9, [least f, height, horizon y, g, w]): fixed in
// the infield 25 m inside the rail, turned to the leader, zoomed to keep it
// PAN[1] px long, f at most PAN[2]. WIDE takes the nearest when it is more
// than PAN[0] m from the leader (1e9: side WIDE only).
const PAN = [60, 14, 1500];
// pc: the camera of the frame, [x, z, unit view vector, f, distance to the
// aim, height, horizon y, screen centre x]; a side unit looks along the
// normal (0, 1) from (cx, 0). One projection draws every view (course()).
let pc = null, vq = null, WX = null, Lo, Hi, SQ, SU, SV, ZM;
function wide(g) {
  let m = 0, e = 1e9;
  for (let i = 7; i < CAMS.length; ++i) {
    const q = pose(CAMS[i][3], CAMS[i][4]), a = pose(g, (DNR + DFR) / 2), x = a[0] - q[0], z = a[1] - q[1];
    if (x * x + z * z < e) e = x * x + z * z, m = i;
  }
  return e > PAN[0] * PAN[0] ? m : 0;
}
// Sets cx and pc for shot m (1: locked on lane l) and returns [f, h, hy].
function shot(m, xs, l, cut) {
  let c = CAMS[m];
  if (m === 1) {
    const f = 24 * DL[l];
    cx = xs[l] - 12.5 * U - (HX - 120) * DL[l] / f;
    c = [f, 3, HY + 6 * HS - 72];
  } else if (m > 6) {
    const q = pose(c[3], c[4]), a = pose(mx.apply(null, xs), (DNR + DFR) / 2), x = a[0] - q[0], z = a[1] - q[1], e = M.sqrt(x * x + z * z);
    pc = [q[0], q[1], x / e, z / e, mn(PAN[2], mx(c[0], PAN[1] * e / 2.4)), e, c[1], c[2], 120];
    return [pc[4], c[1], c[2]];
  } else {
    const tgt = mx.apply(null, xs) - c[3] / c[0];
    cx = mx(c[5], mn(c[6], c[4] ? cut ? tgt : cx + (tgt - cx) * .12 + mx.apply(null, rs.v) * DT * .88 : tgt));
  }
  pc = [cx, 0, 0, 1, c[0], 1e9, c[1], c[2], 120];
  return c;
}
// A runner's inputs: hip x, back y, px per unit, ground y, four hooves.
const rin = (l, X, S, Y, a) => ['r' + l, [X, Y - 6 * S + .6 * S * sin(2 * a), S, Y, X + S * (.5 + 3 * sin(a)), X + S * (.5 + 3 * sin(a + .8)),
  X + S * (8 + 3 * sin(a + 3.3)), X + S * (8 + 3 * sin(a + 4.1))]];
// The screen's feed on its face vr at scale z: a low camera 6 m behind the
// leader, drawn inside vr (course() with the window vr).
function feed(d, xs, z) {
  if (von < 11) return;
  const p = pc;
  pc = [mx.apply(null, xs) - 6, 0, 0, 1, 85 * z, 1e9, 3, vr[1] + 4.8 * z, (vr[0] + vr[2]) / 2];
  WX = vr;
  d.push.apply(d, course(xs, -1, 0));
  pc = p; WX = null;
}
