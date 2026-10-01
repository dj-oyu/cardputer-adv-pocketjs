'use strict'; // ../wrap copy, BGX mode = screen shown (docs/apps/derby-watch.md, surface 1)



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
    const r = rng(f.seed ^ 0x5bd1e995), fm = z8(0), e = [];
    for (let i = 0; i < 8; ++i) { e[i] = f.h[i].st; if (nz) fm[i] = (r() - .5) * FORM; }
    return {t: 0, x: z8(0), v: z8(0), e: e, sb: z8(0), px: z8(0), tc: z8(0), fm: fm, w: z8(0),
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
      let g = s.e[i] <= 0 ? top * FADE : early ? top * (1 - PACE * h.sty) + mn(PK1, gap * PK0) : top * (1 + h.kick * (1 + KS * h.sty));
      if (!early && gap < BG) g += gap * BK;
      if (s.nz) {
        g += s.w[i] = s.w[i] * WR + (s.r() - .5) * WS;
        if (s.r() < .0006) s.sb[i] = .5;
        if (s.sb[i] > 0) s.sb[i] -= dt, g *= .9;
      }
      const a = g - v, up = h.acc * dt;
      v += a > up ? up : a < -dt ? -dt : a;
      if (v > EB * h.top) s.e[i] -= (v / h.top - EB) * (gap < 1 ? WIND : 1) * dt;
      const y = s.x[i] = x + (s.v[i] = v) * dt;
      if (x < D && y >= D) s.tc[i] = t0 + (D - x) / (y - x) * dt, ++s.done;
    }
  }
  const order = s => [0, 1, 2, 3, 4, 5, 6, 7].sort((a, b) => (s.tc[a] || 1e9) - (s.tc[b] || 1e9) || s.x[b] - s.x[a] || a - b);
  // Win chance: softmax of the noise-free finish time (TAU fitted), 20% take.
  function odds(T) {
    const m = mn.apply(null, T), p = [];
    let z = 0, i;
    for (i = 0; i < 8; ++i) z += p[i] = M.exp((m - T[i]) / TAU);
    for (i = 0; i < 8; ++i) p[i] = mx(1.1, mn(99.9, rnd(8 * z / p[i]) / 10));
    return p;
  }
  globalThis.derby = {field: field, race: race, step: step, order: order, odds: odds, D: D, DT: DT, TAU: TAU};


  // ---- Programs as text, one letter per ksn_proc_op (MEGADEMO's form).
  const OPS = 'SIAMNREVPLQBplC', FLD = ['14', '12', '123', '123', '12', '2', '', '23', '235', '235', '2', '23', '123', '123', '25'];
  function prog(src, arg) {
    const c = [], w = src.split(' ');
    for (let i = 0; i < w.length; ++i) {
      const t = w[i], o = OPS.indexOf(t[0]), f = FLD[o], v = t.slice(1).split(','), row = [o, 0, 0, 0, 0, 0];
      for (let j = 0; j < f.length; ++j) row[+f[j]] = v[j][0] === '$' ? arg[+v[j].slice(1)] : +v[j];
      c.push(row);
    }
    return c;
  }
  // [LIGHT, MID, HEAVY]: stand tiers, crowd rows, dots per bay, roof arc
  // segments, rail post and turf stripe spacing (m).
  const KN = [[3, 2, 3, 3, 8, 10], [4, 3, 4, 5, 5, 7], [5, 4, 6, 8, 4, 5]];
  let tier = 1;
  const T = {
    rail: 'I0,0 I1,1 I2,2 I3,3 I4,4 I5,5 I8,6 S6,0 A6,6,0 M7,4,1 A7,7,0 V6,2 l8,7,2 V6,5 l8,7,5 Q4 V0,2 l8,0,3 A0,0,1 E',
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
    map: 'S9,-4 S10,8 S11,12 S12,232 S13,2 V10,11 L12,11,16904 V12,13 L12,11,63488',
    // The screen: its face filled row by row in colour input 6 (the feed
    // is drawn over it), b bezel rings round it, a top light, two legs.
    vis: 'I0,0 I1,1 I2,2 I3,3 I4,4 I5,5 I15,6 S6,-1 S7,1 A8,0,7 A9,2,6 A10,1,7 M11,1,6 A11,11,3 A11,11,6 Q11 V8,10 l15,9,10 A10,10,7 E Q4 V0,1 L2,1,10565 L2,3,10565 L0,3,10565 L0,1,10565 A0,0,6 A1,1,6 A2,2,7 A3,3,7 E V0,1 L2,1,23275 M8,0,6 A8,8,2 S9,.25 M9,8,9 A9,9,0 S11,.5 M8,8,11 A10,9,8 M11,4,6 A12,9,11 V12,3 L12,5,19049 A12,9,4 V12,3 L12,5,19049 A12,10,11 V12,3 L12,5,19049 A12,10,4 V12,3 L12,5,19049',
    // A horse head on (HEAD ON cut): x, ground y, px per unit, coat, silk,
    // lift of each foreleg (px).
    fr: 'I0,0 I1,1 I2,2 I5,5 I6,6 I8,3 I9,4 S15,-1 M5,5,15 A5,5,1 M6,6,15 A6,6,1 S3,-1.2 M3,3,2 A3,3,0 M4,3,15 A4,4,0 A4,4,0 S7,-6 M7,7,2 A7,7,1 V3,7 l8,3,5 V4,7 l8,4,6 M14,2,15 M11,2,15 A11,11,0 A12,0,2 A10,11,14 A13,12,2 S5,-9.5 M5,5,2 A5,5,1 A6,5,2 V10,7 l8,10,6 l8,11,5 l8,12,5 l8,13,6 l8,13,7 l8,10,7 S7,-13 M7,7,2 A7,7,1 V3,5 l9,11,7 l9,12,7 l9,4,5 A7,7,14 p9,0,7 A6,6,2 A5,5,14 A5,5,14 V0,6 l8,0,5'
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
    if (n === 'hd') return [prog('S0,0'), {kind: 'affineQ14Points', x: HD[0], y: HD[1], color: 0xad55, coeff: [16384, 0, 0, 16384, 0, 0]}];
    if (n[0] === 'r' && i >= 0) return [prog(T.runner, [F.h[i].coat, SILK[i], SILK[i] ^ 0x8410])];
    return [prog(T[n], k.concat(1 / k[2]))];
  }
  // HEAD ON's fixed set (rails, finish line, stands, the screen edge on) as
  // one polyline; points from the camera in README.
  const HD = [[120, -155, 120, 425, 120, -155, -63, 323, 425, 120, 85, 159, 120, 100, 142, 120, 184, 166, 166, 166, 156, 156, 166, 166, 184, 184, 189, 189, 196, 196, 204, 204, 214, 214, 227, 227, 240, 227, 227, 240, 227, 227, 120, 184],
    [50, 105, 50, 105, 50, 160, 123, 123, 160, 50, 64, 64, 50, 58, 58, 50, 56, 56, 40, 14, 21, 42, 40, 56, 56, 19, 17, 56, 57, 14, 10, 58, 59, 5, -1, 60, 61, 60, -1, -8, -1, 60, 50, 19]];
  const PAD = ['turf', 'rail', 'stands', 'silk', 'g0', 'g1', 'g2', 'g3', 'g4', 'g5', 'crowd', 'pole'],
    RUN = ['gate', 'r0', 'r1', 'r2', 'r3', 'r4', 'r5', 'r6', 'r7', 'map', 'vis', 'fr', 'hd'];

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
      queue.shift(); ++reg; log('REG ' + n + ' ' + live[n]);
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
  let rs = null, solo = null, od = null, fin = null, photoX = null, replay = 0, ph = z8(0);
  let cx = 0, disp = 0, slow = 0, ld = -1, cm = 0, hold = 0, dl = 0, man = 0, cl = 0, vr = null, von = 0, ro = [0, 1, 2, 3, 4, 5, 6, 7];
  const hex = s => ('0000000' + s.toString(16).toUpperCase()).slice(-8), num = i => 'NO.' + (i + 1),
    th = p => (p + 1) + (['ST', 'ND', 'RD'][p] || 'TH');
  const ST = pocket.storage;
  const save = nop;

  // ---- Cameras [f, height, horizon y]: WIDE, CLOSE (set per lane), FIELD,
  // FINISH (slow motion), PHOTO (along the line: the line is x=120), VISION
  // (low, held within 30 m of the screen).
  const CAMS = [[100, 9.7, 33], 0, [58, 15, 36], [170, 7, 22], [300, 4, 30], [130, 1.6, 84]],
    NAMES = ['WIDE', 'CLOSE', 'FIELD', 'FINISH', '', 'VISION', 'HEAD ON'];
  // The screen (README "Turf vision"): centre x, depth, half width, bottom
  // and top height (m), a 4:1 face.
  const VS = [840, 34, 20, 6, 16], FN = [3, 4, 6], LO = [3, -25, -30, 1, 1, 4, -30, -2, -26, 12, 12, 8];
  // Sets cx for shot m (1: locked on lane l) and returns the camera.
  function shot(m, xs, l, cut) {
    if (m === 1) {
      const f = 24 * DL[l];
      cx = xs[l] - 12.5 * U - (HX - 120) * DL[l] / f;
      return [f, 3, HY + 6 * HS - 72];
    }
    const c = CAMS[m], tgt = mx.apply(null, xs) - (m === 3 ? 640 : 880) / c[0];
    cx = m === 4 ? D : m === 3 ? mn(tgt, D - 6) : cut ? tgt : cx + (tgt - cx) * .12 + mx.apply(null, rs.v) * DT * .88;
    if (m === 5) cx = mx(VS[0] - 30, mn(VS[0] + 30, cx));
    return c;
  }
  // One frame of the course as [plan, inputs], back to front, for camera c
  // at x0. K: the screen's face when this is its feed, a camera 6 m behind
  // the leader, low on the rail: rails and the leading FN[tier] runners,
  // those wholly inside K.
  function course(c, x0, xs, close, gate, K) {
    const f = c[0], h = c[1], hy = c[2], k = KN[tier], d = [], o = K ? (K[0] + K[2]) / 2 : 120, R = K ? K[2] - 1 : 245,
      sx = (w, d0) => o + (w - x0) * f / d0, gy = d0 => hy + h * f / d0, ty = (d0, e) => hy + (h - e) * f / d0;
    const rail = (d0, col) => {
      const p = f / d0, s = k[4] * p, a = sx((K ? M.ceil : flo)((x0 - (o - (K ? K[0] : -5)) / p) / k[4]) * k[4], d0);
      return ['rail', [a, s, ty(d0, 1.1), gy(d0), mx(0, K ? flo((R - a) / s) : mn(flo((700 - a) / s), M.ceil((R - a) / s))), ty(d0, .55), col]];
    };
    let q = f / 40, n;
    if (!K) {
      // Stands and crowd at 40 m, a pillar every 12 m.
      const j = flo((x0 - 130 / q) / 12), a = sx(j * 12, 40), dx = 12 * q;
      n = mn(flo((700 - a) / dx), M.ceil((250 - a) / dx) + 1);
      d.push(['stands', [a, dx, gy(40), -2.4 * q, n, 0, 0, ty(40, 13.5)]],
        ['crowd', [a, dx, gy(40), -2.4 * q, n, j * k[2] * 2.39996 % (2 * PI) - 2 * PI * rnd(n * k[2] * .191), (t >> 3) & 1]]);
      // The screen in front of the stands: dark, a grey flash, then its feed.
      if (vr) {
        const z = (vr[2] - vr[0]) / 120;
        d.push(['vis', [vr[0] - 1, vr[1] - 1, vr[2], vr[3], mx(1, rnd(f / VS[1] * .4)), gy(VS[1]), von < 8 ? 0 : von < 11 ? 0x632c : 0x0866]]);
        if (von > 10) d.push.apply(d, course([85 * z, 3, vr[1] + 4.8 * z], mx.apply(null, xs) - 6, xs, -1, 0, vr));
      }
    }
    d.push(rail(DFR, 0xad55));
    if (!K) {
      // Turf stripes: lines of constant distance, so they meet at the vanishing
      // point; none whose near end is left of -470 (the VM's -480 limit: a
      // close-up of a far lane at LIGHT's 10 m spacing reached -499).
      q = f / DFR;
      const w = k[5], i0 = mx(flo((x0 - 125 / q) / w), M.ceil((x0 - 590 * DNR / f) / w)), xf = sx(i0 * w, DFR), xn = sx(i0 * w, DNR);
      n = mx(0, mn(M.ceil((250 - xf) / (w * q)), flo((700 - xn) / (w * f / DNR))));
      d.push(['turf', [xn, xf, w * f / DNR, w * q, gy(DNR), gy(DFR), n, i0 & 1]]);
      for (let m = 200; m <= D; m += 200) {
        const p = sx(m, DFR), e = m === D;
        if (p > -40 && p < 280)
          d.push(['pole', [p, gy(DFR), ty(DFR, e ? 4 : 2.6), (e ? .55 : .3) * q, e ? sx(m, DNR) : p, gy(e ? DNR : DFR), p, gy(DFR)]]);
      }
      // The gate at 0 m: 9 stall posts, lane boundaries one MUL apart in depth.
      q = f * M.sqrt(Q) / DL[0];
      if (gate && M.abs(x0 * q) < 400) d.push(['gate', [-x0 * q, h * q, (h - 2.6) * q, 1 / Q, hy]]);
    }
    for (let l = 7; l >= 0; --l) {
      if (K && ro.indexOf(l) >= FN[tier]) continue;
      const p = f / DL[l], S = p * U, X = o + (xs[l] - 12.5 * U - x0) * p, Y = gy(DL[l]), a = ph[l];
      if (l === close) {
        const g = (flo(a * 3 / PI) % 6 + 6) % 6;
        d.push(['g' + g, []], ['silk', [SILK[l], rnd(HS * bob(g))]]);
      } else if (K ? X - 2.5 * S >= K[0] && X + 12.5 * S <= R : X > -160 && X < 400)
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
    if (s === 'gate') { drop(['conf']); want(RUN); rs = race(F, 1); ph = [0, 1, 2, 3, 4, 5, 6, 7]; cam = disp = slow = cm = hold = dl = man = von = 0; ld = -1; notes = FANFARE.slice(); }
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
        STY[h.sty] + (od ? ' x' + od[pick] : ''), 'BET ' + stake + '  PTS ' + pts + '   A/D HORSE E/S BET 1 GO'];
      for (let i = 0; i < 8; ++i) R.od[i].setText(tx, od ? od[i] < 10 ? od[i].toFixed(1) : '' + rnd(od[i]) : '-');
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
    const p = c[0] / VS[1];
    vr = [rnd(120 + (VS[0] - VS[2] - cx) * p), rnd(c[2] + (c[1] - VS[4]) * p), rnd(120 + (VS[0] + VS[2] - cx) * p), rnd(c[2] + (c[1] - VS[3]) * p)];
    if (scene !== 'race' || cm > 5 || vr[0] > 239 || vr[2] < 1 || vr[1] < 13 || vr[1] > 134 || vr[2] - vr[0] < 8 || vr[2] - vr[0] > 160) vr = null;
    if (vr) ++von;
    let d = [['hd', []]];
    if (cm > 5 && scene === 'race') {
      // HEAD ON: a still camera 12 m past the line, 2.2 m up, looks back
      // down the course; each horse scaled by its own 1/z (JS divides),
      // the last (farthest) first.
      for (let i = 7; i >= 0; --i) {
        const l = ro[i], q = 400 / (D + 12 - xs[l]), s = .25 * q * sin(ph[l]);
        d.push(['fr', [120 + (DL[l] - 16.5) * q, 50 + 2.2 * q, q / 6, F.h[l].coat, SILK[l], mx(0, s), mx(0, -s)]]);
      }
    } else d = course(c, cx, xs, close, gate);
    H.beginFrame(4);
    for (const e of d.concat(extra)) if (live[e[0]]) H.draw(live[e[0]], e[1]);
    H.commit();
  }
  function frame_() {
    const P = k => k === dk;
    ++t;
    if (camT > 0) --camT;
    // The curtain: wall clock only (attract's nw, fe); the game never reads it.
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
      // A camera key overrides the director for 5 s.
      if (P(',') || P('/') || P('a') || P('d')) { cam = (cam + (P('/') || P('d') ? 1 : 2)) % 3; camT = 45; man = 150; }
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
          if (w !== ld) { if (lead > 500) log('LEAD #' + (w + 1) + ' at ' + rnd(lead) + 'M'); if (lead > 400) dl = 60; ld = w; }
          // The photo: every runner where it was when the winner crossed.
          if (!was && rs.done) photoX = at((rs.tc[w] - rs.t + DT) / DT);
        }
        if (live.gate && lead > 120) drop(['gate']);
        if (rs.done >= 3) { settle(); return enter('photo'); }
      }
      xs = at(slow ? disp : 1);
      if (scene === 'gate') for (let i = 0; i < 8; ++i) if (!live['r' + i]) xs[i] = -999;
      if (hold > 0) --hold;
      if (dl > 0) --dl;
      if (man > 0) --man;
      // The director: a shot for each stretch of the race, a lead change
      // after 400 m cuts to the new leader, a cut is held 45 frames, HEAD ON
      // (a close finish) until the slow motion (README).
      const o = ro = order(rs), m = scene !== 'race' ? 0 : FC[(RT / 360 | 0) % 5];
      if (scene === 'race') {
        ++RT; const b = vr ? 1 : 0, g = 'BGX ' + tier + ' ' + m + ' ' + b;
        bo = b;
        if (g !== lg) log(lg = g);
      } else bo = 0;
      if (m !== cm) { cl = man ? pick : o[0]; cm = m; hold = camT = 45; log('CAM ' + (NAMES[m] || m)); }
      c = shot(m % 6, xs, cl, scene === 'gate' || camT === 45);
      close = m === 1 ? cl : -1;
      gate = live.gate;
      extra = [['map', z8(0)]];
      for (let i = 0; i < 8; ++i) extra[0][1][i] = 8 + mn(rs.x[i], D) * .224;
    } else if (scene === 'photo') {
      c = shot(4, photoX);
      xs = photoX;
      extra = [['photo', [120]]];
      if (t > 110 || t > 20 && P('1')) return enter('res');
    } else if (scene === 'res') {
      if (P('1')) return enter('pad');
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
    paint(c, xs, close, gate, extra);
    const up = tx => {
      hud(tx);
      R.dm.setVisible(tx, dm > 0 && !(t & 16));
      R.fd.setVisible(tx, fa > 0);
      R.fd.setColor(tx, fa);
      // The lettering follows the face (offsets LO from its left or right
      // edge and its top); its text is the HUD's metres and first three.
      const v = !!vr && von > 10 && vr[2] - vr[0] > 99;
      if (R.v && v !== R.vs) for (let i = 0; i < 3; ++i) R.v[i].setVisible(tx, R.vs = v);
      if (v) for (let i = 0; i < 3; ++i) R.v[i].setRect(tx, [vr[i && 2] + LO[i], vr[1] + LO[i + 3], vr[2] + LO[i + 6], vr[1] + LO[i + 9]]);
      if (v && !(t & 3)) { R.v[2].setColor(tx, t & 8 ? 0xff2020ff : 0x401010ff); R.v[0].setText(tx, R.t[1].s + ' ' + R.t[0].s.slice(0, 5)); }
      if (scene === 'photo') {
        // Magnify 2x about the line and the runners' feet.
        const z = mn(1, t / 30), e = z * z * (3 - 2 * z);
        R[0].setRect(tx, [rnd(-120 * e), rnd(-100 * e), rnd(240 + 120 * e), rnd(135 + 35 * e)]);
      }
    };
    if (need) { build(up); need = 0; } else V.patch(up);
  }
  // The script: odds, go; after each result the same race again, the tier
  // moving on every second one (MID, HEAVY, LIGHT).
  let dm = 0, dk = '', fa = 0, RT = 0, bo = 0, lg = '', nr = 0;
  const FC = [0, 1, 2, 5, 6], OFF = [{}, {stands: 1, crowd: 1, rail: 1, turf: 1, pole: 1}, {crowd: 1}];
  globalThis.frame = function (b) {
    if (scene === 'res' && t === 1) ++nr;
    dk = scene === 'pad' && od && t > 20 ? '1' : scene === 'res' && t === 30 && !(nr % 2) ? 'tab' : scene === 'res' && t > 32 ? '1' : '';
    try { frame_(); } catch (e) { log('FRAMEFAIL ' + scene + ' ' + e); throw e; }
  };
  enter('pad');
  log('READY bgcost');

