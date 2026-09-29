// DERBY WATCH: a night race on a 1000 m straight in pseudo-3D line art.
// Why and numbers: apps/derby/README.md, docs/apps/derby-watch.md
(function () {
  'use strict';
  const M = Math, PI = M.PI, sin = M.sin, flo = M.floor, rnd = M.round, mx = M.max, mn = M.min;
  const D = 1000, DT = .05, U = 1 / 6;
  // Race model, tuned by tools/games/tune_derby.mjs (README has the terms).
  const TOP = 16.6, SPR = .2, ST0 = .8, ST1 = 1, KI0 = .02, KI1 = .015, PACE = .015, FORM = .6, WR = .995,
    WS = .02, FADE = .97, EB = .97, PK0 = .03, PK1 = .5, WIND = 1.5, KS = .25, BG = 4, BK = .15, TAU = .23;
  // Lane depths grow geometrically: one MUL walks them in the VM (gate).
  const Q = 1.085, DL = [], DNR = 11, DFR = 22.6;
  for (let j = 0; j < 8; ++j) DL.push(12 * M.pow(Q, j));
  const POOL = 'BIT DASH,HEX HOOF,NULL MOON,BAUD STAR,LOOP KID,CACHE HIT,TIN KITE,OPCODE,PIXEL RUN,STACK TOP,BYTE MARE,MOSS DRUM'.split(',');
  const SILK = [0xffff, 0x8c71, 0xf800, 0x237f, 0xffe0, 0x07e0, 0xfd20, 0xf81f], STY = ['FRONT', 'STALK', 'CLOSE'];
  const z8 = v => [v, v, v, v, v, v, v, v];
  function rng(s) {
    return function () {
      s = s + 0x6d2b79f5 | 0;
      let t = M.imul(s ^ s >>> 15, 1 | s);
      t = t + M.imul(t ^ t >>> 7, 61 | t) ^ t;
      return ((t ^ t >>> 14) >>> 0) / 4294967296;
    };
  }
  function field(seed) {
    const r = rng(seed), p = POOL.slice(), h = [];
    for (let i = 0; i < 8; ++i)
      h.push({n: p.splice(flo(r() * p.length), 1)[0], top: TOP + SPR * r(), st: ST0 + ST1 * r(), kick: KI0 + KI1 * r(),
        sty: flo(r() * 3), re: .25 * r(), acc: 5 + 2 * r(), coat: [0xdd8c, 0xc460, 0xbdf7, 0xef5b][flo(r() * 4)]});
    return {seed: seed, h: h};
  }
  // Arithmetic only, no Math.sin/exp: a seed replays bit for bit anywhere.
  // Per race a form offset the odds cannot see, and a slow random walk.
  function race(f, nz) {
    const r = rng(f.seed ^ 0x5bd1e995), fm = z8(0);
    if (nz) for (let i = 0; i < 8; ++i) fm[i] = (r() - .5) * FORM;
    return {t: 0, x: z8(0), v: z8(0), e: f.h.map(h => h.st), sb: z8(0), px: z8(0), tc: z8(0), fm: fm, w: z8(0),
      r: r, nz: nz, done: 0};
  }
  function step(f, s, dt) {
    const t0 = s.t, xl = mx.apply(null, s.x);
    s.t += dt;
    for (let i = 0; i < 8; ++i) {
      const h = f.h[i], x = s.x[i], top = h.top + s.fm[i], gap = xl - x, early = x < D * (.5 + .08 * h.sty);
      let v = s.v[i];
      s.px[i] = x;
      if (s.t < h.re) continue;
      // Early the field bunches and the leader pays for the wind; late, a
      // runner within BG m of the lead digs in.
      let g = early ? top * (1 - PACE * h.sty) + mn(PK1, gap * PK0) : top * (1 + h.kick * (1 + KS * h.sty));
      if (s.e[i] <= 0) g = top * FADE;
      if (!early && gap < BG) g += gap * BK;
      if (s.nz) {
        g += s.w[i] = s.w[i] * WR + (s.r() - .5) * WS;
        if (s.r() < .0006) s.sb[i] = .5;
        if (s.sb[i] > 0) { s.sb[i] -= dt; g *= .9; }
      }
      const a = g - v, up = h.acc * dt;
      v += a > up ? up : a < -dt ? -dt : a;
      if (v > EB * h.top) s.e[i] -= (v / h.top - EB) * (gap < 1 ? WIND : 1) * dt;
      s.v[i] = v;
      s.x[i] = x + v * dt;
      if (x < D && s.x[i] >= D) { s.tc[i] = t0 + (D - x) / (s.x[i] - x) * dt; ++s.done; }
    }
  }
  const order = s => [0, 1, 2, 3, 4, 5, 6, 7].sort((a, b) => (s.tc[a] || 1e9) - (s.tc[b] || 1e9) || s.x[b] - s.x[a] || a - b);
  // Win chance: softmax of the noise-free finish time (TAU fitted), 20% take.
  function odds(T) {
    const m = mn.apply(null, T), p = T.map(t => M.exp((m - t) / TAU)), z = p.reduce((a, b) => a + b);
    return p.map(q => mx(1.1, mn(99.9, rnd(8 * z / q) / 10)));
  }
  globalThis.derby = {field: field, race: race, step: step, order: order, odds: odds, D: D, DT: DT, TAU: TAU};
  if (typeof pocket === 'undefined') return;

  // ---- Programs as text, one letter per ksn_proc_op (MEGADEMO's form).
  const OPS = 'SIAMNREVPLQBplC', FLD = ['14', '12', '123', '123', '12', '2', '', '23', '235', '235', '2', '23', '123', '123', '25'];
  function prog(src, arg) {
    const c = [], w = src.split(' ');
    for (let i = 0; i < w.length; ++i) {
      const t = w[i], o = OPS.indexOf(t[0]), f = FLD[o], v = t.slice(1).split(','), row = [o, 0, 0, 0, 0, 0];
      for (let j = 0; j < f.length; ++j) row[+f[j]] = v[j][0] === '$' ? arg[+v[j].slice(1)] : +v[j];
      c.push(row);
    }
    if (c.length > 64) throw RangeError('64 instructions');
    return c;
  }
  // [LIGHT, MID, HEAVY]: stand tiers, crowd rows, dots per bay, roof arc
  // segments, rail post and turf stripe spacing (m).
  const KN = [[3, 2, 3, 3, 8, 10], [4, 3, 4, 5, 5, 7], [5, 4, 6, 8, 4, 5]];
  let tier = 1;
  const T = {
    rail: 'I0,0 I1,1 I2,2 I3,3 I4,4 I5,5 I8,6 S6,0 S7,239 V6,2 l8,7,2 V6,5 l8,7,5 Q4 V0,2 l8,0,3 A0,0,1 E',
    turf: 'I0,0 I1,1 I2,2 I3,3 I4,4 I5,5 I6,6 I9,7 S10,288 M9,9,10 S8,512 A8,8,9 S11,-1 S12,1312 Q6 V0,4 l8,1,5 A0,0,2 A1,1,3 M8,8,11 A8,8,12 E',
    stands: 'I8,0 I9,1 I10,2 I11,3 I12,4 I7,7 S15,0 S0,0 S2,239 A1,10,15 R$0 V0,1 L2,1,21130 A1,1,11 E I1,7 A3,7,11 A3,3,11 A5,3,15 Q12 A0,8,15 A2,8,15 A4,8,9 A6,8,9 V8,10 L8,7,31727 C$3,50712 A8,8,9 E',
    crowd: 'I8,0 I9,1 I10,2 I11,3 I12,4 I14,6 S15,12650 M14,14,15 S15,46496 A14,14,15 S15,105642 S1,1.5 S2,2.39996 S4,-1 S3,$6 M3,3,9 S13,0 S7,.5 M5,11,7 A5,5,10 R$1 I0,0 I6,5 A6,6,13 Q12 R$2 N7,6 M7,7,1 A7,7,0 p14,7,5 A0,0,3 A6,6,2 M14,14,4 A14,14,15 E E A5,5,11 S7,.9 A13,13,7 E',
    runner: 'I0,0 I1,1 I2,2 I3,3 S4,2.5 M2,2,4 S4,-1 M4,4,2 A5,0,4 A6,0,2 A7,6,2 A8,7,2 A9,8,2 A10,9,2 A11,1,2 A12,1,4 A13,12,4 V5,11 L0,1,$0 L8,1,$0 L9,13,$0 L10,12,$0 L9,12,$0 I14,4 V0,1 L14,3,$0 I14,5 V0,1 L14,3,$0 I14,6 V8,1 L14,3,$0 I14,7 V8,1 L14,3,$0 V7,1 L7,12,$1 L8,13,$1 L9,12,$1 P8,13,$2',
    gate: 'I0,0 I1,1 I2,2 I3,3 I4,4 S5,120 R9 A6,5,0 A7,4,1 A8,4,2 L6,8,40147 L6,7,40147 V6,8 M0,0,3 M1,1,3 M2,2,3 E',
    pole: 'I8,0 I9,1 I10,2 I11,3 V8,9 L8,10,63488 S15,-1 M12,11,15 A13,10,12 S14,-1.33 M14,14,11 S15,0 A0,8,12 A1,13,15 A2,0,15 A3,13,14 A4,8,11 A5,3,15 A6,4,15 A7,13,15 C6,65535 S15,-1 M14,14,15 A3,13,14 A5,13,14 C6,65535 I0,4 I1,5 I2,6 I3,7 V0,1 L2,3,65535 S15,1 A0,0,15 A2,2,15 V0,1 L2,3,65535',
    photo: 'I0,0 S1,13 S2,127 V0,1 L0,2,65535 S3,1 A0,0,3 V0,1 L0,2,63488 S9,0 S10,239 V9,1 L10,1,50712 V9,2 L10,2,50712 S4,0 S5,128 S6,133 S7,6 S8,131 R8 R4 V4,5 L4,8,50712 A4,4,7 E V4,5 L4,6,65535 A4,4,7 E',
    conf: 'I2,0 I1,1 S0,0 S9,1 S8,-1 S6,65504 S7,129055 S11,1.7 S12,.037 S13,2.1 S14,.00041 R96 A0,0,9 B0,1 M5,0,11 N3,5 S5,110 M3,3,5 S5,120 A3,3,5 M5,0,14 A5,5,12 M5,5,2 M4,0,13 A4,4,5 N4,4 S5,64 M4,4,5 S5,67 A4,4,5 V3,4 A5,3,9 A10,4,9 A10,10,9 l6,5,10 M6,6,8 A6,6,7 E',
    // The jockey over the close-up horse (points baked for HX,HY; README):
    // silk colour input 0, bob (px) input 1, white cap by CUBIC.
    silk: 'I8,0 I9,1 S0,113 S1,96 A1,1,9 S2,116 S3,92 A3,3,9 S4,109 S5,84 A5,5,9 S6,119 S7,74 A7,7,9 V0,1 l8,2,3 l8,4,5 l8,6,7 S0,127 S1,80 A1,1,9 l8,0,1 S0,120 S1,71 A1,1,9 S2,120 S3,66 A3,3,9 S4,128 S5,66 A5,5,9 S6,128 S7,71 A7,7,9 C5,65535',
    map: 'S9,-4 S10,8 S11,12 S12,232 S13,2 V10,11 L12,11,16904 V12,13 L12,11,63488'
  };
  for (let k = 0; k < 8; ++k) T.map += ' I0,' + k + ' S1,' + (3 + k) + ' V0,1 A0,0,9 L0,1,' + SILK[k];
  // The close-up horse: 6 gallop frames of one polyline (typed points, half
  // pixels), fixed on screen with the hip at HX,HY and 4 px per unit.
  const HX = 92, HY = 88, HS = 4, bob = k => .35 * sin(2 * PI * k / 3);
  // Units, y down, hip at 0,0; a pair 20+o,l is a leg of phase o and
  // segment length l from the point before it (knee, hoof, back up).
  const BODY = [-2.4, 3.2, -1.6, 1.4, -.6, 0, .3, -.4, 3.8, .3, 7, -.9, 8.3, -3.1, 9.9, -5, 9.8, -6, 10.5, -4.9,
    12.7, -2.3, 12.5, -1.6, 10.4, -2.4, 9.3, -2, 8.6, .8, 7.4, 2.2, 23.14, 1.9, 23.64, 1.9, 4, 2.7, 1.2, 2, 20, 2,
    20.5, 2, -.5, 1.2, -.6, 0];
  function gallop(k) {
    const x = [], y = [], ph = PI * k / 3, b = bob(k), put = (u, v) => { x.push(rnd(8 * u)); y.push(rnd(8 * v)); };
    let jx = 0, jy = 0;
    for (let i = 0; i < BODY.length; i += 2) {
      const u = BODY[i], l = BODY[i + 1];
      if (u < 20) { put(jx = u, jy = l + b); continue; }
      const a = .55 * sin(ph + u - 20), kx = jx + l * sin(a), ky = jy + l * M.cos(a), a2 = a - .9 * mx(0, sin(ph + u - 18.6));
      put(kx, ky); put(kx + l * sin(a2), ky + l * M.cos(a2)); put(kx, ky); put(jx, jy);
    }
    return {kind: 'affineQ14Points', x: x, y: y, color: 0xef5b, coeff: [8192, 0, 0, 8192, HX * 16384, HY * 16384]};
  }
  let F = null;
  function spec(n) {
    const k = KN[tier], i = +n[1];
    if (n[0] === 'g' && i >= 0) return [prog('S0,0'), gallop(i)];
    if (n[0] === 'r' && i >= 0) return [prog(T.runner, [F.h[i].coat, SILK[i], SILK[i] ^ 0x8410])];
    return [prog(T[n], k.concat(1 / k[2]))];
  }
  const PAD = ['turf', 'rail', 'stands', 'silk', 'g0', 'g1', 'g2', 'g3', 'g4', 'g5', 'crowd', 'pole'],
    RUN = ['gate', 'r0', 'r1', 'r2', 'r3', 'r4', 'r5', 'r6', 'r7', 'map'];

  const V = pocket.kasane, H = V.procedural, K = pocket.input.keys, log = m => console.log('DERBY ' + m);
  // Frames now, while the heap still has holes that size (MEGADEMO's lesson).
  H.beginFrame(0);
  const res = H.resource(), live = {}, queue = [];
  let reg = 0;
  // One plan a frame from frame 1 (info() is null while evaluating), only
  // above MEGADEMO's free-heap margin: a register dips the heap up to 14 KB.
  function load() {
    const n = queue[0];
    if (!n) return;
    try {
      if (pocket.memory && pocket.memory.info().internalFreeBytes < 22528) return;
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
  const done = () => { busy = 0; }, nop = () => {};
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
  let rs = null, solo = null, od = null, fin = null, photoX = null, replay = 0, ph = z8(0);
  let cx = 0, disp = 0, slow = 0, ld = -1;
  const hex = s => ('0000000' + s.toString(16).toUpperCase()).slice(-8), num = i => 'NO.' + (i + 1),
    th = p => (p + 1) + (['ST', 'ND', 'RD'][p] || 'TH');
  const ST = pocket.storage;
  function save() { if (dm) return; try { ST.set('derby.v1', {v: 1, pts: pts, race: raceNo}).then(nop, nop); } catch (e) {} }
  try {
    ST.get('derby.v1').then(r => {
      const v = r && r.value;
      if (v && v.v === 1 && v.pts > 0 && v.race > 0 && scene === 'pad') {
        pts = mx(50, v.pts | 0); raceNo = v.race | 0; enter('pad');
        log('LOADED points=' + pts + ' race=' + raceNo);
      }
    }, nop);
  } catch (e) {}

  // ---- Cameras [f, height, horizon y]: WIDE, CLOSE (set per lane), FIELD,
  // FINISH (slow motion), PHOTO (along the line: the line is x=120).
  const CAMS = [[100, 9.7, 33], 0, [58, 15, 36], [170, 7, 22], [300, 4, 30]], NAMES = ['WIDE', 'CLOSE', 'FIELD', 'FINISH'];
  // Sets cx for shot m (1: locked on lane l) and returns the camera.
  function shot(m, xs, l, cut) {
    if (m === 1) {
      const f = 24 * DL[l];
      cx = xs[l] - 12.5 * U - (HX - 120) * DL[l] / f;
      return [f, 3, HY + 6 * HS - 72];
    }
    const c = CAMS[m], tgt = mx.apply(null, xs) - (m === 3 ? 640 : 880) / c[0];
    cx = m === 4 ? D : m === 3 ? mn(tgt, D - 6) : cut ? tgt : cx + (tgt - cx) * .12 + mx.apply(null, rs.v) * DT * .88;
    return c;
  }
  // One frame of the course as [plan, inputs], back to front.
  function course(c, xs, close, gate) {
    const f = c[0], h = c[1], hy = c[2], k = KN[tier], d = [], sx = (w, d0) => 120 + (w - cx) * f / d0,
      gy = d0 => hy + h * f / d0, ty = (d0, e) => hy + (h - e) * f / d0;
    // Stands and crowd at 40 m, a pillar every 12 m.
    let q = f / 40, j = flo((cx - 130 / q) / 12), x0 = sx(j * 12, 40), dx = 12 * q;
    let n = mn(flo((700 - x0) / dx), M.ceil((250 - x0) / dx) + 1);
    d.push(['stands', [x0, dx, gy(40), -2.4 * q, n, 0, 0, ty(40, 13.5)]],
      ['crowd', [x0, dx, gy(40), -2.4 * q, n, j * k[2] * 2.39996, (t >> 3) & 1]]);
    const rail = (d0, col) => {
      const p = f / d0, a = sx(flo((cx - 125 / p) / k[4]) * k[4], d0), s = k[4] * p;
      return ['rail', [a, s, ty(d0, 1.1), gy(d0), mx(0, mn(flo((700 - a) / s), M.ceil((245 - a) / s))), ty(d0, .55), col]];
    };
    d.push(rail(DFR, 0xad55));
    // Turf stripes: lines of constant distance, so they meet at the vanishing point.
    q = f / DFR;
    const w = k[5], i0 = flo((cx - 125 / q) / w), xf = sx(i0 * w, DFR), xn = sx(i0 * w, DNR);
    n = mx(0, mn(M.ceil((250 - xf) / (w * q)), flo((700 - xn) / (w * f / DNR))));
    d.push(['turf', [xn, xf, w * f / DNR, w * q, gy(DNR), gy(DFR), n, i0 & 1]]);
    for (let m = 200; m <= D; m += 200) {
      const p = sx(m, DFR), e = m === D;
      if (p > -40 && p < 280)
        d.push(['pole', [p, gy(DFR), ty(DFR, e ? 4 : 2.6), (e ? .55 : .3) * q, e ? sx(m, DNR) : p, gy(e ? DNR : DFR), p, gy(DFR)]]);
    }
    // The gate at 0 m: 9 stall posts, lane boundaries one MUL apart in depth.
    q = f * M.sqrt(Q) / DL[0];
    if (gate && M.abs(cx * q) < 400) d.push(['gate', [-cx * q, h * q, (h - 2.6) * q, 1 / Q, hy]]);
    for (let l = 7; l >= 0; --l) {
      const p = f / DL[l], S = p * U, X = 120 + (xs[l] - 12.5 * U - cx) * p, Y = gy(DL[l]), a = ph[l];
      if (l === close) {
        const g = (flo(a * 3 / PI) % 6 + 6) % 6;
        d.push(['g' + g, []], ['silk', [SILK[l], rnd(HS * bob(g))]]);
      } else if (X > -160 && X < 400)
        d.push(['r' + l, [X, Y - 6 * S + .6 * S * sin(2 * a), S, Y, X + S * (.5 + 3 * sin(a)), X + S * (.5 + 3 * sin(a + .8)),
          X + S * (8 + 3 * sin(a + 3.3)), X + S * (8 + 3 * sin(a + 4.1))]]);
    }
    d.push(rail(DNR, 0xffff));
    return d;
  }

  // ---- Scenes: pad (pick), gate (runner plans load one a frame: horses
  // enter the stalls), race, photo (still), res (result).
  function enter(s) {
    scene = s; t = 0; need = 1;
    if (s === 'pad') { drop(['conf']); want(PAD); replay = 0; F = field((0x3e1b7 + M.imul(raceNo, 0x9e3779b9)) >>> 0); solo = race(F, 0); od = null; }
    if (s === 'gate') { drop(['conf']); want(RUN); rs = race(F, 1); ph = [0, 1, 2, 3, 4, 5, 6, 7]; cam = disp = slow = 0; ld = -1; notes = FANFARE.slice(); }
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
      R.dm = tt([212, 109, 238, 120, 0xfffb96ff, 4], 'DEMO');
      up(tx);
    });
  }
  function hud(tx) {
    let s;
    if (scene === 'pad') {
      const h = F.h[pick];
      s = [(dm ? '' : 'RACE ' + raceNo + '  ') + '1000M STRAIGHT  SEED ' + hex(F.seed), num(pick) + ' ' + h.n,
        STY[h.sty] + (od ? ' x' + od[pick] : ''), 'BET ' + stake + '  PTS ' + pts + '   A/D HORSE E/S BET 1 GO'];
      for (let i = 0; i < 8; ++i) R.od[i].setText(tx, od ? od[i] < 10 ? od[i].toFixed(1) : '' + rnd(od[i]) : '-');
      R.sel.setRect(tx, [1 + 30 * pick, 13, 31 + 30 * pick, 24]);
      [(h.top - TOP) / SPR, (h.st - ST0) / ST1, (h.kick - KI0) / KI1].forEach((v, i) =>
        R.bar[i].setRect(tx, [180, 43 + 12 * i, 182 + rnd(52 * mx(0, mn(1, v))), 49 + 12 * i]));
    } else if (scene === 'res') {
      const o = fin.o, p = o.indexOf(pick);
      s = [(replay ? 'REPLAY  ' : 'WINNER  ') + num(o[0]) + ' ' + F.h[o[0]].n + '  ' + fin.mg,
        '1ST ' + (o[0] + 1) + '  2ND ' + (o[1] + 1) + '  3RD ' + (o[2] + 1) + '  4TH ' + (o[3] + 1),
        'YOUR ' + num(pick) + ' ' + th(p) + (replay ? '' : '  ' + (fin.dp < 0 ? '' : '+') + fin.dp),
        'PTS ' + pts + '   1 NEXT RACE   R REPLAY'];
    } else {
      const o = rs ? order(rs) : [0, 1, 2], lead = rs ? mx.apply(null, rs.x) : 0, ph2 = scene === 'photo';
      s = [(o[0] + 1) + '-' + (o[1] + 1) + '-' + (o[2] + 1) + '   ' + num(pick) + ' ' + th(o.indexOf(pick)),
        scene === 'gate' ? 'GATE' : mx(0, rnd(D - lead)) + 'M', camT > 0 ? NAMES[slow ? 3 : cam] : '',
        ph2 ? (t < 50 ? (t & 8 ? 'PHOTO' : '') : num(fin.o[0])) : '', ph2 && t >= 50 ? fin.mg : ''];
    }
    for (let i = 0; i < s.length; ++i) R.t[i].setText(tx, s[i]);
    R.dm.setVisible(tx, dm > 0 && !(t & 16));
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
    log('FINISH race=' + raceNo + ' seed=' + hex(F.seed) + ' order=' + o.map(i => i + 1) + ' t=' + o.map(i => rs.tc[i]) + ' margin=' + mg);
    if (!replay) { log('RESULT pick=' + (pick + 1) + ' place=' + (o.indexOf(pick) + 1) + ' delta=' + dp + ' points=' + pts); save(); }
    notes = (o[0] === pick ? WIN : LOSE).slice();
  }

  function frame_() {
    const P = k => dk === 0 ? K.pressed(k) : k === dk;
    ++t;
    if (camT > 0) --camT;
    if (P('tab')) { tier = (tier + 1) % 3; drop(['stands', 'crowd']); want(['stands', 'crowd']); log('TIER ' + tier); }
    load();
    let xs, c, l = pick, close = -1, gate = 0, extra = [];
    if (scene === 'pad') {
      if (P('a') || P('d')) { pick = (pick + (P('d') ? 1 : 7)) % 8; if (A) A.cue('move'); }
      stake = mx(50, mn(stake + (P('e') ? 50 : P('s') ? -50 : 0), 500, pts));
      for (let i = 0; i < 30 && solo.done < 8; ++i) step(F, solo, .25);
      if (!od && solo.done === 8) { od = odds(solo.tc); log('ODDS ' + od); }
      if (P('1') && od) { log('PICK ' + (pick + 1) + ' stake=' + stake + ' odds=' + od[pick]); if (A) A.cue('accept'); return enter('gate'); }
    } else if (scene === 'gate' || scene === 'race') {
      if (P(',') || P('/') || P('a') || P('d')) { cam = (cam + (P('/') || P('d') ? 1 : 2)) % 3; camT = 45; }
      if (scene === 'gate' && (!queue.length && t > 40 || t > 300)) { log('GO' + (queue.length ? ' LOADSTALL' : '')); return enter('race'); }
      if (scene === 'race') {
        const lead = mx.apply(null, rs.x);
        if (!slow && lead > D - 20) { slow = 1; camT = 60; log('SLOW'); }
        // Slow motion shows each sim step over 4 frames: the sim never
        // changes, so the camera cannot change the race.
        disp += slow ? .25 : 1;
        for (; disp >= 1 && rs.done < 3; --disp) {
          const was = rs.done;
          step(F, rs, DT);
          for (let i = 0; i < 8; ++i) ph[i] += rs.v[i] * DT / 6.5 * 2 * PI;
          const w = order(rs)[0];
          if (w !== ld) { if (lead > 500) log('LEAD #' + (w + 1) + ' at ' + rnd(lead) + 'M'); ld = w; }
          // The photo: every runner where it was when the winner crossed.
          if (!was && rs.done) { const u = (rs.tc[w] - rs.t + DT) / DT; photoX = rs.x.map((x, i) => rs.px[i] + (x - rs.px[i]) * u); }
        }
        if (live.gate && lead > 120) drop(['gate']);
        if (rs.done >= 3) { settle(); return enter('photo'); }
      }
      const u = slow ? disp : 1;
      xs = rs.x.map((x, i) => rs.px[i] + (x - rs.px[i]) * u);
      if (scene === 'gate') for (let i = 0; i < 8; ++i) if (!live['r' + i]) xs[i] = -999;
      c = shot(slow ? 3 : cam, xs, pick, scene === 'gate' || camT === 45);
      close = !slow && cam === 1 ? pick : -1;
      gate = live.gate;
      extra = [['map', rs.x.map(x => 8 + mn(x, D) * .224)]];
    } else if (scene === 'photo') {
      c = shot(4, photoX);
      xs = photoX;
      extra = [['photo', [120]]];
      if (t > 110 || t > 20 && P('1')) return enter('res');
    } else if (scene === 'res') {
      if (P('1')) { ++raceNo; save(); return enter('pad'); }
      if (P('r')) { replay = 1; return enter('gate'); }
      l = fin.o[0];
      extra = [['conf', [t, mn(96, t)]]];
    }
    if (!xs) {
      // Paddock warm-up and the winner's canter: a lone horse, camera locked.
      xs = z8(-999);
      xs[l] = 60 + t * .25;
      ph[l] += .25 / 6.5 * 2 * PI;
      c = shot(1, xs, l);
      close = l;
    }
    H.beginFrame(4);
    for (const e of course(c, xs, close, gate).concat(extra)) if (live[e[0]]) H.draw(live[e[0]], e[1]);
    H.commit();
    const up = tx => {
      hud(tx);
      if (scene === 'photo') {
        // Magnify 2x about the line and the runners' feet.
        const z = mn(1, t / 30), e = z * z * (3 - 2 * z);
        R[0].setRect(tx, [rnd(-120 * e), rnd(-100 * e), rnd(240 + 120 * e), rnd(135 + 35 * e)]);
      }
    };
    if (need) { build(up); need = 0; } else V.patch(up);
  }
  // ---- Demo: DEMO_IDLE_S of no key at the paddock (wall clock), then the
  // game plays itself, its keys fed to P(). Player state is set aside; races
  // 1e6+n are the demo's seeds; silent (A = null); no save.
  const DEMO_IDLE_S = 15, DEMO_RES = 150, GK = [...'adesr1,/', 'tab'];
  let dm = 0, dk = 0, dn = 0, idle = 0, kp = '', bk;
  function demo(on, n) {
    if (!on) { [pts, raceNo, pick, stake, A] = bk; notes = []; drop(RUN); drop(['photo']); }
    log('DEMO ' + (on ? 'START ' : 'END ') + [pts, raceNo, pick, stake]);
    if (on) { bk = [pts, raceNo, pick, stake, A]; A = null; raceNo = 1e6 + ++dn; pick = 0; stake = 100; }
    dm = on; dk = ''; idle = n;
    enter('pad');
  }
  // A key ends the demo and is spent doing so (dk '' matches no key).
  function attract(b) {
    const k = K.down().join(), n = pocket.time ? pocket.time.now() : 0;
    let hit = b || k && k !== kp;
    for (const x of GK) hit = hit || K.pressed(x);
    kp = k;
    if (!dm) {
      dk = 0;
      if (!idle || hit || k || scene !== 'pad') idle = n;
      else if (n - idle >= DEMO_IDLE_S * 1e3) demo(1, n);
      return;
    }
    if (hit || scene === 'res' && t > DEMO_RES) return demo(0, n);
    dk = '';
    // Favourite, then second favourite, one A/D step a time, then 1.
    if (scene === 'pad' && od && t > 45 && !(t % 12)) {
      const g = [0, 1, 2, 3, 4, 5, 6, 7].sort((a, c) => od[a] - od[c] || a - c)[1 - dn % 2];
      dk = g === pick ? '1' : (g - pick + 8) % 8 > 4 ? 'a' : 'd';
    }
    if (scene === 'race' && t % 240 === 120) dk = '/';
  }
  globalThis.frame = function (b) {
    if (b & 0x2000) { if (dm) demo(0); save(); return log('SAVE points=' + pts + ' race=' + raceNo); }
    try { attract(b); frame_(); } catch (e) { log('FRAMEFAIL ' + scene + ' ' + e); throw e; }
    sound();
  };
  enter('pad');
  log('READY tones=' + !!A);
})();
