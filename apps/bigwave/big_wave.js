// BIG WAVE: ride a peeling wave in third person, lines only (kasane.procedural).
// E pump S brake A/D turn ; pop . tuck , / carve 1 start 2-4 load tab pause.
// Why: apps/bigwave/README.md
(function () {
  'use strict';
  const M = Math, f32 = M.fround, rnd = M.round, cl = (v, a, b) => v < a ? a : v > b ? b : v;
  // ---- Tuning: physics, course and difficulty per set (docs/apps/big-wave.md).
  const K = {
    vw: [9, 10.5, 12], H: [5, 6.5, 8], len: [320, 360, 400], rock: [70, 55, 42], sec: [120, 90, 70],
    rid: [160, 120, 95], acc: 5, brake: 6, drag: .05, grav: 6, turn: 1.9, carve: 3.6, cost: 2.5,
    relax: 1.4, head: 1.05, face: 7, pop: 5.5, g: 13, land: .8, air: 4, tube: 7, pocket: 22, sh: 34,
    taper: 40, hmin: .3, tuck: .6, grace: 8, hitX: 1.3, hitP: .12, lives: 3, wipe: 48, inv: 45,
    pts: [12, 80, 4, 40, 400]
  };
  // Camera: vanishing point, focal px, rider distance, eye height, offset to the
  // flats (m), nearest slice (m), slice step (m). Load tiers: far slice, CUBIC and foam steps.
  const C = {vx: 160, vy: 56, F: 150, cb: 8, ov: 2.2, ol: 2.6, dn: 5.2, D: 1.8};
  const TIER = [[44, 5, 4], [54, 6, 5], [64, 7, 6]];
  // Face profile in wave heights: P1 (L,0), P2 (L,V), lip (l,v) + curl*(dl,dv).
  const PF = {L: -1, V: 1.25, l: -1.35, v: 1.05, dl: 1.45, dv: -.5, t: .62};
  const SETS = [
    {n: 'MORNING', bg: 0x0845, cu: 0xafff, sh: 0x455f, se: 0x2b5a, sk: 0xfd8a, fo: 0xffff},
    {n: 'NOON', bg: 0x000b, cu: 0x07ff, sh: 0x03df, se: 0x0272, sk: 0xffff, fo: 0xffff},
    {n: 'DUSK', bg: 0x1805, cu: 0x5fff, sh: 0xb01f, se: 0x600c, sk: 0xfd20, fo: 0xffdf}];
  const RIDERS = [0xffe0, 0xf9c7, 0x87f0];

  // Programs as text, one letter per ksn_proc_op then its fields; $x is an argument.
  const OPS = 'SIAMNREVPLQBplC', FL = ['14', '12', '123', '123', '12', '2', '', '23', '235', '235', '2', '23',
    '123', '123', '25'];
  function prog(src, arg) {
    const c = src.split(' ').map(t => {
      const v = t.slice(1).split(',').map(x => x[0] === '$' ? arg[x[1]] : +x);
      const o = OPS.indexOf(t[0]), f = FL[o], row = [o, 0, 0, 0, 0, 0];
      for (let j = 0; j < f.length; ++j) row[+f[j]] = v[j];
      row[4] = f32(row[4]);
      return row;
    });
    if (c.length > 64) throw RangeError('64 instructions');
    return c;
  }
  // Slice loop: no divide in the VM, so y = 1/depth steps by y *= 1-u+u^2-u^3, u = D*y.
  const Y = ' S13,$D M14,8,13 M12,14,15 S13,1 A12,12,13 M12,12,14 M12,12,15 A12,12,13 M12,12,14' +
    ' M12,12,15 A12,12,13 M8,8,12 E', A0 = 'I13,0 M10,8,13 I13,1 M11,8,13 ',
    TV = 'I13,2 M12,8,13 A0,12,4 I13,3 M12,8,13 A1,12,5 ',
    TS = 'I13,2 M12,8,13 S13,$x A0,12,13 I13,3 M12,8,13 S13,$y A1,12,13 ',
    S12 = 'S13,$L M12,13,10 A2,0,12 M12,13,11 A3,1,12 S13,$V M12,13,11 A4,2,12 S13,$W M12,13,10 A5,3,12 ',
    LC = 'L6,7,$c C$n,$c I13,7 A9,9,13', W0 = 'I8,4 I9,6 S15,-1 I13,5 Q13 ';
  const T = {
    cu: W0 + A0 + TS + S12 + 'S13,$d M14,9,13 S13,$l A14,14,13 M12,14,10 A6,0,12 M12,14,11 A7,1,12' +
      ' S13,$e M14,9,13 S13,$v A14,14,13 M12,14,11 A6,6,12 M12,14,10 M12,12,15 A7,7,12 ' + LC + Y,
    sh: W0 + 'M14,8,9 I13,0 M10,14,13 I13,1 M11,14,13 ' + TS + S12 + 'S13,$l M12,13,10 A6,0,12' +
      ' M12,13,11 A7,1,12 S13,$v M12,13,11 A6,6,12 S13,$u M12,13,10 A7,7,12 ' + LC + Y,
    se: 'I8,4 S15,-1 S4,$x S5,$y I13,5 Q13 ' + A0 + TV + 'S13,$f M12,13,10 A2,0,12 M12,13,11 A3,1,12' +
      ' L0,1,$t V2,3 L0,1,$s S13,97 M6,8,13 I13,6 A6,6,13 N6,6 S13,.45 M6,6,13 S13,.5 A6,6,13 S13,$f' +
      ' M6,6,13 M12,6,10 A2,0,12 M12,6,11 A3,1,12 P2,3,$g' + Y,
    sk: 'I8,4 S4,$x S5,$y ' + A0 + TV + 'S13,$h I12,0 M12,12,13 A2,4,12 I12,1 M12,12,13 A3,5,12 V4,5' +
      ' L2,3,$k S6,$r R$m V4,5 M12,6,10 A2,0,12 M12,6,11 A3,1,12 L2,3,$t S13,$q M6,6,13 E',
    fo: 'I8,4 S15,-1 S4,$x S5,$y I9,6 I13,5 Q13 ' + A0 + TV + 'V0,1 S6,$l R$n S13,1.9 M7,6,13 A7,7,9' +
      ' N7,7 S13,1 A7,7,13 I13,7 M7,7,13 M12,6,10 A2,0,12 M12,6,11 A3,1,12 M12,7,11 A2,2,12 M12,7,10' +
      ' M12,12,15 A3,3,12 L2,3,$c S13,$d A6,6,13 E S13,2.3 A9,9,13' + Y,
    ri: 'I0,0 I1,1 I2,2 I3,3 I13,4 M4,3,13 S12,-1 M5,2,13 M5,5,12 I14,5 S13,.3 M2,2,13 M3,3,13 M8,2,12' +
      ' M9,3,12 A6,0,8 A7,1,9 V6,7 S13,.85 M10,13,4 A10,10,0 M11,13,5 A11,11,1 l14,10,11 A6,0,2 A7,1,3' +
      ' l14,6,7 A6,0,8 A7,1,9 l14,6,7 V10,11 S13,1.85 M6,13,4 A6,6,0 M7,13,5 A7,7,1 l14,6,7 S13,1.25' +
      ' M6,13,4 A6,6,0 M7,13,5 A7,7,1 A6,6,8 A6,6,8 A7,7,9 A7,7,9 V6,7 A6,6,2 A6,6,2 A6,6,2 A6,6,2' +
      ' A7,7,3 A7,7,3 A7,7,3 A7,7,3 l14,6,7',
    ro: 'I8,0 I9,1 I10,2 I11,3 S15,-1 M12,10,15 S13,-1.2 M0,13,10 A0,0,8 M1,13,11 A1,1,9 S13,-1' +
      ' M2,13,10 A2,2,8 M3,13,11 A3,3,9 S13,1.3 M14,13,11 A2,2,14 M14,13,12 A3,3,14 S13,.9 M4,13,10' +
      ' A4,4,8 M5,13,11 A5,5,9 S13,1.5 M14,13,11 A4,4,14 M14,13,12 A5,5,14 S13,1.2 M6,13,10 A6,6,8' +
      ' M7,13,11 A7,7,9 C6,35953 L0,1,35953',
    ln: 'I0,0 I1,1 I2,2 I3,3 I4,4 V0,1 l4,2,3', sp: 'S0,0'
  };
  // Rider, stand and tuck: board then body, one path (px, feet at 0,0).
  const BODY = [[0, 6, 4, 2, 3, -5, 0, -12, -3, -5, -4, 2, 0, 6, 1, 1, 5, -8, 0, -16, -5, -9, -1, -5, -5, -9,
    0, -16, -1, -27, -13, -22, -1, -27, 11, -25, -1, -27, -1, -29, -4, -31, -1, -36, 2, -31, -1, -29],
  [0, 6, 4, 2, 3, -5, 0, -12, -3, -5, -4, 2, 0, 6, 1, 1, 6, -5, 0, -11, -6, -6, -1, -5, -6, -6, 0, -11, -2,
    -19, -12, -13, -2, -19, 9, -15, -2, -19, -2, -21, -5, -23, -2, -27, 1, -23, -2, -21]];
  const Q = 16384, BIN = .2, NB = 7;
  function sprite(i) {
    const b = i % NB, L = (b - 3) * BIN, r = -.45 * L, a = .55 * L, pts = BODY[(i / NB) | 0];
    const k = C.F / C.cb, ox = -k * C.ol, oy = k * C.ov, c = M.cos(r), s = M.sin(r);
    const px = C.vx + ox * c - oy * s, qy = C.vy + ox * s + oy * c;
    const x = [], y = [];
    for (let j = 0; j < pts.length; j += 2) { x.push(pts[j]); y.push(pts[j + 1]); }
    return {kind: 'affineQ14Points', x: x, y: y, color: 0xffe0, coeff: [rnd(M.cos(a) * Q),
      rnd(-M.sin(a) * Q), rnd(M.sin(a) * Q), rnd(M.cos(a) * Q), rnd(px * Q), rnd(qy * Q)]};
  }
  // Arguments per plan; wave plans change with the set (colors, heights).
  function args(k, set, tier) {
    const P = SETS[set], H = K.H[set], tr = TIER[tier], F = C.F;
    const a = {D: C.D, x: C.vx, y: C.vy, L: PF.L, V: PF.V, W: -PF.V, l: PF.l, v: PF.v, u: -PF.v, d: PF.dl,
      e: PF.dv, n: tr[1]};
    if (k === 'cu') a.c = P.cu;
    if (k === 'sh') a.c = P.sh;
    if (k === 'se') { a.f = 8; a.t = P.cu; a.s = P.se; a.g = P.fo; }
    if (k === 'sk') { a.h = 250 / F; a.k = P.sk; a.r = 2; a.m = 5; a.t = P.se; a.q = 1.6; }
    if (k === 'fo') { a.l = .25 * H; a.n = tr[2]; a.d = -1.55 * H / (tr[2] - 1); a.c = P.fo; }
    return a;
  }
  const SETK = ['cu', 'sh', 'se', 'sk', 'fo'];

  // ---- World: the curl at xc peels along +x; the rider is at x on face fraction p.
  const S = {st: 0, set: 0, tier: 1, t: 0, x: 0, xc: -14, p: .4, v: 9, h: 0, z: 0, vz: 0, lean: 0, roll: 0,
    tuck: 0, score: 0, best: 0, lives: 3, tube: 0, air: 0, hit: 0, inv: 0, wt: 0, why: '', pause: false,
    course: null, cx: 0, lc: 0, vc: 0, co: 1, si: 0};
  function course(k) {
    let r = 20260929 + k * 7919;
    const rand = () => (r = (M.imul(r, 1664525) + 1013904223) >>> 0) / 4294967296, L = K.len[k];
    const c = {rocks: [], secs: [], riders: []};
    for (let x = 70; x < L - 30; x += K.rock[k] * (.7 + .6 * rand())) c.rocks.push({x: x, p: .04 + .24 * rand()});
    for (let x = 110; x < L - 40; x += K.sec[k] * (.8 + .4 * rand())) c.secs.push({x: x, w: 6 + 6 * rand()});
    for (let x = 60; x < L - 40; x += K.rid[k] * (.8 + .4 * rand()))
      c.riders.push({x: x, v: 5 + 1.5 * rand(), p: .3 + .35 * rand(), ph: 6 * rand(), c: RIDERS[c.riders.length % 3]});
    return c;
  }
  const H = () => K.H[S.set];
  function curlAt(x) {
    const q = x - S.xc;
    let c = q < K.tube ? 1 : q < K.pocket ? 1 - .8 * (q - K.tube) / (K.pocket - K.tube) :
      q < K.sh - 4 ? .2 * (1 - (q - K.pocket) / (K.sh - 4 - K.pocket)) : 0;
    const s = S.course.secs;
    for (let i = 0; i < s.length; ++i) {
      const a = secAmp(s[i]);
      if (!a) continue;
      const u = x - s[i].x, w = s[i].w;
      if (u > -4 && u < w + 4) c += .8 * a * (u < 0 ? 1 + u / 4 : u > w ? 1 - (u - w) / 4 : 1);
    }
    return M.min(c, 1);
  }
  // A section breaks only once the full-height zone reaches it (README).
  function secAmp(s) { return cl((K.sh - 4 - C.D - (s.x + s.w + 4 - S.xc)) / 8, 0, 1); }
  function heightAt(x) {
    const q = x - S.xc - K.sh;
    return H() * (q < 0 ? 1 : q < K.taper ? 1 - (1 - K.hmin) * q / K.taper : K.hmin);
  }
  // Face point at fraction p of the profile with curl c and height h: [l, v] m.
  function face(p, c, h) {
    const t = p * PF.t, u = 1 - t, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
    return [h * ((b1 + b2) * PF.L + b3 * (PF.l + c * PF.dl)), h * (b2 * PF.V + b3 * (PF.v + c * PF.dv))];
  }
  function power(q) {
    return q < 0 ? .3 : q < K.tube ? .75 : q < K.pocket ? 1 : q < K.sh ? 1 - .5 * (q - K.pocket) / (K.sh - K.pocket) :
      M.max(.1, .5 - .4 * (q - K.sh) / K.taper);
  }

  // ---- Plans: handles by key; a queue registers one per frame (a decode is 12-20 ms).
  const P = typeof pocket !== 'undefined' && pocket.kasane && pocket.kasane.procedural;
  const PL = {}, queue = [];
  function spec(k, set, tier) {
    if (k[0] === 's' && k[1] === 'p') return [prog(T.sp, {}), sprite(+k.slice(2))];
    return [prog(T[k], SETK.indexOf(k) < 0 ? {} : args(k, set, tier)), null];
  }
  function load(k, set, tier) { queue.push([k, set, tier]); }
  function pump() {
    const e = queue.shift();
    if (!e) return;
    const s = spec(e[0], e[1], e[2]);
    if (PL[e[0]]) P.unregister(PL[e[0]]);
    PL[e[0]] = s[1] ? P.register(s[0], s[1]) : P.register(s[0]);
  }
  function loadSet(k) { SETK.forEach(n => load(n, k, S.tier)); }

  // ---- Keys and sound.
  const KEYS = P && pocket.input && pocket.input.keys;
  const cap = n => { try { return pocket.capabilities.get(n).supported; } catch (e) { return false; } };
  const AU = P && cap('audio.tone') ? pocket.audio : null;
  let sq = [], busy = 0;
  function snd(l) { if (AU && sq.length < 6) sq = sq.concat(l); }
  function sound() {
    busy -= 33;
    if (busy > 0 || !sq.length) return;
    const n = sq.shift();
    busy = n[1] + 25;
    try { AU.tone({frequencyHz: n[0], durationMs: n[1], gain: n[2]}).catch(() => 0); } catch (e) { busy = 0; }
  }

  // ---- Rules.
  const log = s => console.log('BIGWAVE ' + s);
  function start(set, keep) {
    Object.assign(S, {set: set, x: 0, xc: -14, p: .4, v: K.vw[set], h: 0, z: 0, vz: 0, lean: 0, tube: 0,
      air: 0, hit: 0, inv: 0, course: course(set), st: 1});
    if (!keep) { S.score = 0; S.lives = K.lives; }
    log('RIDE set=' + set + ' tier=' + S.tier + ' lives=' + S.lives);
  }
  function goSet(set, keep) {
    if (S.set !== set || !PL.cu) loadSet(set);
    S.set = set; S.course = course(set); S.st = 3; S.wt = 0; S.keep = keep;
  }
  function wipe(why) {
    if (S.inv > 0 || S.st !== 1) return;
    S.st = 2; S.wt = 0; S.why = why; S.wx = S.x; S.wf = face(S.p, curlAt(S.x), heightAt(S.x)); --S.lives;
    snd([[180, 300, .6], [120, 250, .5]]);
    log('WIPE ' + why + ' x=' + S.x.toFixed(1) + ' lives=' + S.lives + ' score=' + rnd(S.score));
  }
  function held(k) { return KEYS ? KEYS.held(k) : false; }
  function hit(k) { return KEYS ? KEYS.pressed(k) : false; }
  function physics(inp, dt) {
    const s = S, air = s.z > 0;
    let tr = (inp.a ? K.turn : 0) - (inp.d ? K.turn : 0) + (inp.cl ? K.carve : 0) - (inp.cr ? K.carve : 0);
    if (air) tr *= .3;
    s.h = cl(tr ? s.h + tr * dt : s.h - s.h * K.relax * dt, -K.head, K.head);
    s.lean += (cl(tr / K.carve, -1, 1) * .6 - s.lean) * .2;
    s.tuck = inp.dn ? 1 : 0;
    const q = s.x - s.xc, pw = K.drag * (K.vw[s.set] * 1.08) ** 2 * power(q) * (.5 + .5 * M.sin(M.PI * cl(s.p, 0, 1)));
    let a = pw - K.grav * M.sin(s.h) - K.drag * s.v * s.v - s.tuck * .5;
    if (inp.e) a += K.acc;
    if (inp.s) a -= K.brake;
    if (inp.cl || inp.cr) a -= K.cost;
    if (air) a = -.2 * K.drag * s.v * s.v;
    s.v = cl(s.v + a * dt, 4, 22);
    s.x += s.v * M.cos(s.h) * dt;
    s.xc += K.vw[s.set] * dt;
    s.p += s.v * M.sin(s.h) * dt / K.face;
    if (s.p < 0) { s.p = 0; if (s.h < -.2) s.h = -.2; }
    if (s.p > 1) {
      if (!air && s.v * M.sin(s.h) > K.air) { s.vz = s.v * M.sin(s.h) * .6; s.z = .01; snd([[784, 60, .4]]); }
      s.p = 1; if (s.h > 0 && !air) s.h = 0;
    }
    if (inp.pop && !air) { s.vz = K.pop; s.z = .01; snd([[660, 50, .4]]); }
    if (s.z > 0) {
      s.air++; s.z += s.vz * dt; s.vz -= K.g * dt;
      if (s.z <= 0) {
        s.z = 0;
        if (M.abs(s.h) > K.land) return wipe('LAND');
        s.score += K.pts[2] * s.air; s.air = 0; snd([[330, 40, .4]]);
      }
    }
    if (s.inv > 0) --s.inv;
    // Zones: caught, barrel (tuck and stay low), pocket, shoulder.
    const inTube = q >= 0 && q < K.tube || secAt(s.x);
    if (q < 0) return wipe('CAUGHT');
    if (inTube && !air) {
      if (s.tuck && s.p < K.tuck) {
        if (!s.tube) snd([[880, 90, .4]]);
        s.tube++; s.hit = 0; s.score += K.pts[1] * dt;
        if (!(s.tube % 15)) snd([[1047, 25, .25]]);
      } else if (++s.hit > K.grace) return wipe('LIP');
    } else {
      if (s.tube > 20) log('TUBE ' + s.tube);
      s.tube = 0; s.hit = 0;
      if (q < K.pocket) s.score += K.pts[0] * dt * s.v / K.vw[s.set];
      else s.score += 2 * dt;
    }
    if ((inp.cl || inp.cr) && s.p > .85 && !air && !(s.t % 8)) s.score += K.pts[3] / 4;
    // Obstacles: rocks near the trough, other riders on the face.
    const c = s.course;
    if (!air || s.z < .6) {
      for (let i = 0; i < c.rocks.length; ++i)
        if (M.abs(s.x - c.rocks[i].x) < K.hitX && M.abs(s.p - c.rocks[i].p) < K.hitP) return wipe('ROCK');
      for (let i = 0; i < c.riders.length; ++i) {
        const r = rider(c.riders[i]);
        if (M.abs(s.x - r[0]) < K.hitX && M.abs(s.p - r[1]) < K.hitP) return wipe('RIDER');
      }
    }
    if (s.x >= K.len[s.set]) {
      s.score += K.pts[4] * (s.set + 1);
      snd([[523, 90, .4], [659, 90, .4], [784, 160, .4]]);
      log('SET ' + s.set + ' CLEAR score=' + rnd(s.score));
      if (s.set < 2) goSet(s.set + 1, 1); else { s.st = 4; s.best = M.max(s.best, rnd(s.score)); log('CLEAR score=' + rnd(s.score)); }
    }
  }
  function secAt(x) {
    const s = S.course.secs;
    for (let i = 0; i < s.length; ++i) if (secAmp(s[i]) > .5 && x > s[i].x && x < s[i].x + s[i].w) return true;
    return false;
  }
  // Riders wait on the face and drop in once the player is 35 m away.
  function rider(r) {
    if (r.t0 === undefined && r.x - S.x < 35) r.t0 = S.t;
    const t = r.t0 === undefined ? 0 : (S.t - r.t0) / 30;
    return [r.x + r.v * t, cl(r.p + .15 * M.sin(.9 * t + r.ph), .05, .9)];
  }
  // Title autopilot: hold the pocket so the attract loop shows the wave.
  function auto() {
    const q = S.x - S.xc;
    return {e: q < 12, s: q > 17, a: S.p < .35, d: S.p > .55};
  }

  // ---- Frame: camera, then draws back to front.
  function camera() {
    const s = S, f = face(s.p, curlAt(s.x), heightAt(s.x));
    s.roll += (-.45 * s.lean - s.roll) * .5;
    s.co = M.cos(s.roll); s.si = M.sin(s.roll);
    s.cx = s.x - C.cb; s.lc = f[0] + C.ol; s.vc = f[1] + C.ov + s.z; s.fw = f;
  }
  function proj(x, l, v, far) {
    const d = x - S.cx;
    if (d < C.dn || d > far) return null;
    const k = C.F / d, a = l - S.lc, b = v - S.vc;
    return [C.vx + k * (a * S.co + b * S.si), C.vy + k * (a * S.si - b * S.co), k];
  }
  function draws(D) {
    const s = S, F = C.F, co = s.co, si = s.si, far = TIER[s.tier][0], d = C.D;
    const w = [-F * (s.lc * co + s.vc * si), -F * (s.lc * si - s.vc * co)], e = [F * co, F * si];
    const m0 = M.ceil((s.cx + C.dn) / d), m1 = M.floor((s.cx + far) / d), y = m => 1 / (m * d - s.cx);
    const add = (k, i) => { if (PL[k]) D.push([PL[k], i]); };
    add('sk', e.concat(w, [1 / C.dn]));
    add('se', e.concat(w, [y(m0), m1 - m0 + 1, s.t * .21]));
    if (s.st === 3) return;
    // Wave runs: slices grouped between breakpoints, each run overlaps the last
    // slice of the previous one so the lip rail stays connected.
    const bp = [s.xc + K.tube, s.xc + K.pocket, s.xc + K.sh - 4, s.xc + K.sh, s.xc + K.sh + K.taper];
    s.course.secs.forEach(c => { if (secAmp(c)) bp.push(c.x - 4, c.x, c.x + c.w, c.x + c.w + 4); });
    bp.sort((a, b) => a - b);
    const ms = M.max(m0, M.ceil(s.xc / d)), H = K.H[s.set], ex = e.map(v => v * H);
    let a = ms, j = 0;
    while (a <= m1) {
      while (j < bp.length && bp[j] <= a * d) ++j;
      let b = a;
      while (b < m1 && (j >= bp.length || (b + 1) * d < bp[j])) ++b;
      const s0 = a > ms ? a - 1 : a, n = b - s0 + 1, x0 = s0 * d, x1 = b * d, sh = a * d >= s.xc + K.sh;
      const f = sh ? heightAt : curlAt, q0 = f(x0), dq = n > 1 ? (f(x1) - q0) / (n - 1) : 0;
      add(sh ? 'sh' : 'cu', (sh ? e : ex).concat(w, [y(s0), n, q0, dq]));
      a = b + 1;
    }
    // Whitewater behind the curl, and the splash of a wipeout.
    const fb = M.min(m1, M.floor(s.xc / d));
    if (fb >= m0) add('fo', e.concat(w, [y(m0), fb - m0 + 1, s.t * .4, .12 * H]));
    if (s.st === 2 && s.wt < 30) {
      const a0 = M.max(m0, M.ceil((s.wx - 2) / d));
      add('fo', e.concat(w, [y(a0), 3, s.t * .7, .3 + s.wt * .02]));
    }
    // Obstacles, far to near.
    const ob = [], c = s.course;
    c.rocks.forEach(r => { const f2 = face(r.p, curlAt(r.x), heightAt(r.x)), p = proj(r.x, f2[0], f2[1], far);
      if (p) ob.push([p[2], 'ro', [p[0], p[1], p[2] * co, p[2] * si]]); });
    c.riders.forEach(r => { const q = rider(r), f2 = face(q[1], curlAt(q[0]), heightAt(q[0])), p = proj(q[0], f2[0], f2[1], far);
      if (p) ob.push([p[2], 'ri', [p[0], p[1], p[2] * co, p[2] * si, 1, r.c]]); });
    const fin = K.len[s.set], pf = proj(fin, 3, 0, far), pt = proj(fin, 3, 3, far);
    if (pf && pt) ob.push([pf[2], 'ln', [pf[0], pf[1], pt[0], pt[1], 0xf800]]);
    ob.sort((a, b) => a[0] - b[0]).forEach(o => { if (o[2][0] > -150 && o[2][0] < 390) add(o[1], o[2]); });
    // The rider: roll bin x pose; a shadow while airborne; tumbling when wiped out.
    const pw = proj(s.x, s.fw[0], s.fw[1], 99);
    if (s.st === 2) {
      const ang = s.wt * .35, k = pw ? pw[2] : 20, p = proj(s.wx, s.wf[0], s.wf[1], 99);
      if (p && s.wt < 36) add('ri', [p[0], p[1] - s.wt, k * M.cos(ang), k * M.sin(ang), 1, 0xffe0]);
      return;
    }
    if (s.z > 0 && pw) add('ln', [pw[0] - 7, pw[1], pw[0] + 7, pw[1], 0x2104]);
    if (s.inv > 0 && (s.inv & 4)) return;
    const b = cl(rnd(s.lean / BIN) + 3, 0, NB - 1);
    add('sp' + (b + s.tuck * NB), []);
  }

  const V = P && pocket.kasane;
  let res, R = {}, shown = {}, built = 0, fr = 0;
  const FULL = [0, 0, 240, 135];
  function ui(tx) {
    tx.background(255);
    tx.image({resource: res, bounds: FULL, clip: FULL, sourceWidth: 240, sourceHeight: 135});
    const T2 = (k, b, f, c, n) => { R[k] = tx.text({bounds: b, text: ' ', capacity: n, font: f, color: c}); };
    T2('a', [4, 2, 120, 12], 'caption', 0xffffffff, 20);
    T2('b', [124, 2, 238, 12], 'caption', 0xffe08cff, 20);
    T2('m', [0, 22, 240, 40], 'display', 0xfffb96ff, 20);
    T2('s', [0, 116, 240, 126], 'caption', 0x01cdfeff, 38);
    shown = {};
  }
  const TXT = {a: 'score', b: 'info', m: 'msg', s: 'sub'};
  function hud() {
    if (fr & 3 && S.st === shown.st && !S.pause) return;
    const s = S, o = {}, set = SETS[s.set].n, pad = n => String(1e6 + rnd(n)).slice(1);
    o.a = s.st ? pad(s.score) + '  x' + s.lives : 'BEST ' + pad(s.best);
    o.b = !s.st ? 'LOAD ' + (s.tier + 2) : set + ' ' + (s.st === 3 ? '' : rnd(M.min(s.x, K.len[s.set])) + '/') + K.len[s.set] + 'm';
    o.m = s.pause ? 'PAUSE' : ['BIG WAVE', s.tube > 5 ? 'TUBE!' : s.hit ? 'TUCK!' : s.z > .5 ? 'AIR' : '', 'WIPEOUT',
      'SET ' + (s.set + 1), 'CLEAR!', 'GAME OVER'][s.st];
    o.s = ['1 START  2-4 LOAD', s.tube > 5 ? 'TUBE ' + (s.tube / 30).toFixed(1) + 's' : '', s.why,
      set + ' ' + K.H[s.set] + 'M', 'SCORE ' + rnd(s.score) + '  1 AGAIN', '1 RETRY'][s.st];
    const ch = Object.keys(o).filter(k => o[k] !== shown[k]);
    if (!ch.length || fr - built < 2) return;
    V.patch(tx => ch.forEach(k => {
      const w = k === 'm' ? 12 : 6, x = k === 'm' || k === 's' ? rnd(120 - o[k].length * w / 2) : 0;
      if (x) R[k].setRect(tx, [x, k === 'm' ? 22 : 116, 240, k === 'm' ? 40 : 126]);
      R[k].setText(tx, o[k] || ' '); shown[k] = o[k];
    }));
    shown.st = s.st;
  }

  function frame(buttons) {
    if (buttons & 0x2000) { log('BACK best=' + M.max(S.best, rnd(S.score)) + ' score=' + rnd(S.score)); return; }
    ++fr;
    const s = S;
    for (let t = 2; t <= 4; ++t) if (hit('' + t) && t - 2 !== s.tier && (s.st === 0 || s.st > 3)) {
      s.tier = t - 2; loadSet(s.set); log('TIER ' + s.tier);
    }
    if (hit('tab') && (s.st === 1 || s.st === 2)) s.pause = !s.pause;
    const go = hit('1');
    if (!s.pause) {
      ++s.t;
      if (s.st === 0) { physics(auto(), 1 / 30); if (s.x > 200) { s.x -= 200; s.xc -= 200; } if (go) start(0); }
      else if (s.st === 1) physics({e: held('e'), s: held('s'), a: held('a'), d: held('d'), cl: held(','),
        cr: held('/'), dn: held('.'), pop: hit(';')}, 1 / 30);
      else if (s.st === 2) {
        s.v *= .96; s.x += s.v / 30; s.xc += K.vw[s.set] / 30; s.z = 0;
        if (++s.wt > K.wipe) {
          if (s.lives > 0) { const x = s.x; start(s.set, 1); s.x = x; s.xc = x - 14; s.inv = K.inv; }
          else { s.st = 5; s.best = M.max(s.best, rnd(s.score)); log('OVER score=' + rnd(s.score)); }
        }
      } else if (s.st === 3) { s.xc += 1; if (++s.wt > 45 && !queue.length) start(s.set, s.keep); }
      else if (go) goSet(0, 0);
    }
    pump();
    if (!s.pause) {
      camera();
      const D = [];
      draws(D);
      P.beginFrame(SETS[s.set].bg);
      for (let i = 0; i < D.length; ++i) P.draw(D[i][0], D[i][1]);
      P.commit();
    }
    if (!built) { V.replace(ui); built = fr; } else hud();
    if (AU) {
      if (s.st === 1 && !(s.t % 70)) snd([[55 + 12 * s.set, 200, .25]]);
      sound();
    }
  }

  globalThis.bigWave = {S: S, PL: PL, queue: queue, curlAt: curlAt};
  if (!P) return;
  res = P.resource();
  loadSet(0); load('ln');
  while (queue.length) pump();
  for (let i = 0; i < 14; ++i) load('sp' + i);
  load('ro'); load('ri');
  S.course = course(0);
  log('READY plans=' + Object.keys(PL).length + ' tone=' + !!AU);
  globalThis.frame = frame;
})();
