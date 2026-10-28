// MEGADEMO chunk 2: Act II's scenes as plans and per-frame inputs, and
// procMegademo (what the host tests read).
'use strict';
const Q14 = 16384, rnd = Math.round, M = Math;
function affine(x, y, s, t, tx, ty, col) {
  const c = s * M.cos(t) * Q14, n = s * M.sin(t) * Q14;
  return {kind: 'affineQ14Points', x: x, y: y, color: col,
    coeff: [rnd(c), rnd(-n), rnd(n), rnd(c), tx * Q14, ty * Q14]};
}
// The cat's ship: a head with two ears, seen from behind.
const SHIP = [0, -4, 5, -12, 7, -5, 13, 0, 10, 5, 5, 8, 2, 6, 0, 9, -2, 6, -5, 8, -10, 5,
  -13, 0, -7, -5, -5, -12, 0, -4];
// The ship in 8 roll bins (a rotated sprite table).
function points(b) {
  const x = [], y = [];
  for (let i = 0; i < SHIP.length; i += 2) { x.push(SHIP[i]); y.push(SHIP[i + 1]); }
  return affine(x, y, 2, (b - 3.5) * .14, 120, 110, WH);
}
const Q0 = [[1.2, .95], [-.8, .95], [-.8, -1.55], [1.2, -1.55]];
const cmul = (p, z) => [p[0] * z[0] - p[1] * z[1], p[0] * z[1] + p[1] * z[0]];
const zp = (r, d, e) => [M.pow(r, e) * M.cos(d * e), M.pow(r, e) * M.sin(d * e)];
const lerp = (p, q, f) => [p[0] + (q[0] - p[0]) * f, p[1] + (q[1] - p[1]) * f];
// TWIST: frames, rails, arches, planks, 2nd rails, 2 wall courses.
function corrList(k) {
  const n = k.n;
  return [['fr', n - k.kf, VI], ['ra', n, GR], ['ar', n - k.ka, k.s, WH], ['ru', n * k.m, OR, OR + WH],
    ['ra', n, GR], ['ra', n, BL], ['ra', n, BL]];
}
// The corridor: every point of a cross-section is multiplied by z per frame.
function corrDraws(k, t, len, sp) {
  const r = k.r, w = M.sin(M.PI * t / len), dl = k.D / k.n * w * w;
  const ph = (t * sp) % 1, g = M.pow(r, 1 - ph) * 265;
  const an = .5 * M.sin(2 * M.PI * t / len) - ph * dl;
  const c = M.cos(an) * g, s = M.sin(an) * g;
  const P = Q0.map(q => [q[0] * c - q[1] * s, q[0] * s + q[1] * c]);
  const z = zp(r, dl, 1), zf = zp(r, dl, 1 / k.m), zk = zp(r, dl, k.kf), za = zp(r, dl, k.ka);
  const A = cmul([(P[2][0] - P[1][0]) / 2, (P[2][1] - P[1][1]) / 2], za);
  const d = [[0, cmul(P[0], zk).concat(cmul(P[1], zk), cmul(P[2], zk), z)],
    [1, P[0].concat(P[1], z)], [3, P[0].concat(P[1], zf)]];
  d.push([4, P[2].concat(P[3], z)], [5, lerp(P[1], P[2], 1 / 3).concat(lerp(P[0], P[3], 1 / 3), z)],
    [6, lerp(P[1], P[2], 2 / 3).concat(lerp(P[0], P[3], 2 / 3), z)]);
  d.push([2, cmul(P[2], za).concat(cmul(P[3], za), A, z)]);
  return d;
}
// Roll of the ZENITH ship (a barrel roll in frames 40..63) and pitch.
function flight(t) {
  const T = 2 * M.PI * t / 96;
  let roll = .55 * M.sin(T);
  if (t >= 40 && t < 64) { const u = (t - 40) / 24; roll += 2 * M.PI * u * u * (3 - 2 * u); }
  const c = M.cos(roll), s = M.sin(roll), hz = 8 * M.sin(2 * T);
  return {roll: roll, c: c, s: s, H: [120 - hz * s, 67 + hz * c, -s, c]};
}
// CORE: an icosahedral shell opens round a pulsing core (cube and
// octahedron); the climax adds grey flashes, scan noise and a short dolly
// zoom, each eased in cubically towards frame 100. The 3D is projected
// here; the plans only draw lines.
// Icosahedron: vertices as sign/golden-ratio codes (i = -1, p/q = +/-phi),
// faces as hex vertex triples; the cube and octahedron are made from bits.
const IV = [], IFS = '0b505101707a0ab1595b4ba2a7671839434232636838949524b62a867981';
{
  const p = (1 + M.sqrt(5)) / 2, n = M.sqrt(1 + p * p), C = {i: -1, '0': 0, '1': 1, p: p, q: -p};
  const S = 'ip01p0iq01q00ip01p0iq01qp0ip01q0iq01';
  for (let i = 0; i < 36; ++i) IV.push(C[S[i]] / n);
}
const ramp = (t, a, b) => M.max(0, M.min(1, (t - a) / (b - a))), smooth = u => u * u * (3 - 2 * u);
// Projection state for this frame: yaw, pitch, camera distance D.
let Ycy, Ysy, Pcp, Psp, Dd;
// Rotate (yaw, then pitch), project at scale s, append x, y to o; returns z
// (> 0 is away from the camera). A petal past the lens must not blow up.
function pj(x, y, z, s, o) {
  const x1 = x * Ycy + z * Ysy, z0 = z * Ycy - x * Ysy, y1 = y * Pcp - z0 * Psp, z1 = y * Psp + z0 * Pcp;
  const f = s * Dd / M.max(Dd + z1, Dd * .4);
  o.push(120 + x1 * f, 67 + y1 * f);
  return z1;
}
// A quad (or two segments) through vertices named by hex digits: cube
// vertex i is the bits of i, octahedron vertex i the axis i % 3 signed by i < 3.
function quad(plan, ids, oct, s) {
  const o = [];
  for (let j = 0; j < 4; ++j) {
    const i = parseInt(ids[j], 16);
    if (oct) pj(i % 3 ? 0 : i < 3 ? 1 : -1, i % 3 === 1 ? i < 3 ? 1 : -1 : 0, i % 3 === 2 ? i < 3 ? 1 : -1 : 0, s, o);
    else pj(i & 1 ? 1 : -1, i & 2 ? 1 : -1, i & 4 ? 1 : -1, s, o);
  }
  return [plan, o];
}
function core(k, t) {
  // Clock g: steps every 3 frames from 86 (stop-motion), stops at 100.
  // At 112 the shell falls in (im), at 115 it bursts (e). Negative frames:
  // the stop and the burst.
  const g = t < 86 ? t : t < 100 ? 86 + 3 * ((t - 86) / 3 | 0) : 100, step = t >= 86 && t < 100 && g === t;
  const neg = t === 100 || t === 101 || t === 115 || t === 116, im = smooth(ramp(t, 112, 115));
  const crit = ramp(g, 60, 100), dz = ramp(g, 84, 100) ** 2, e = ramp(t, 115, 133), ee = 1 - (1 - e) ** 3;
  const sc = (18 + 44 * smooth(ramp(g, 0, 40))) * (1 - .5 * im), o0 = smooth(ramp(g, 40, 56)) + .6 * smooth(ramp(g, 76, 100));
  const op = o0 * (1 - im) + 3.6 * ee, tw = o0 * (.5 + 1.5 * crit) * (1 - im) + 5 * ee;
  const ya = g * .03 + 2 * crit ** 3, pa = .5 + g * .011, d = [], iv = neg ? 0xffff : 0;
  let cost = 0;
  // Grey flashes: one frame each time 9 u^3 crosses an integer, u over 60..86.
  const fl = t === 60 || t > 60 && t < 86 && (9 * crit ** 3 | 0) > (9 * ramp(t - 1, 60, 100) ** 3 | 0);
  // The dolly zoom snaps back in the fall.
  Ycy = M.cos(ya); Ysy = M.sin(ya); Pcp = M.cos(pa); Psp = M.sin(pa); Dd = 3.2 - 1.6 * dz * (1 - im);
  for (let i = 0; i < 20; ++i) {
    // Shrink each face, push it out along its normal, twist it like a petal.
    let cx = 0, cy = 0, cz = 0, zs = 0, far = 0;
    for (let j = 0; j < 3; ++j) {
      const v = 3 * parseInt(IFS[3 * i + j], 16);
      cx += IV[v] / 3; cy += IV[v + 1] / 3; cz += IV[v + 2] / 3;
    }
    const l = op * (.8 + .3 * (i % 4)) / M.hypot(cx, cy, cz), a = tw * (i & 1 ? 1 : -1), ca = M.cos(a), sa = M.sin(a), o = [];
    for (let j = 0; j < 3; ++j) {
      const v = 3 * parseInt(IFS[3 * i + j], 16), h = (1 - .35 * M.min(op, 1.6)) * (1 - .6 * ee);
      const x = cx + (IV[v] - cx) * h + cx * l, y = cy + (IV[v + 1] - cy) * h + cy * l, z = cz + (IV[v + 2] - cz) * h + cz * l;
      zs += pj(x * ca + z * sa, y, z * ca - x * sa, sc, o);
      if (M.abs(o[2 * j] - 120) > 200 || M.abs(o[2 * j + 1] - 67) > 140) far = 1e4;
    }
    // Lines are not clipped and cost their length (8192 a frame): shards
    // off the screen, or past 4000 steps in the burst, are not drawn.
    for (let j = 0; j < 6; j += 2)
      far += M.max(M.abs(o[j] - o[(j + 2) % 6]), M.abs(o[j + 1] - o[(j + 3) % 6]));
    if (far >= 1e4 || e > 0 && (cost += far) > 4000) continue;
    o.push((zs > 0 ? BL : i % 3 ? VI : WH) ^ iv);
    d.push([0, o]);
  }
  // The core: cube (green) and octahedron (orange), counter-rotating,
  // pulsing; the falling shell crushes it.
  const cs = sc * smooth(ramp(g, 40, 56)) * (1 + .15 * M.sin(g * .5)) * (1 + .5 * crit) * (1 - im);
  if (cs > 1) d.push(quad(1, '0132', 0, cs * .33), quad(1, '4576', 0, cs * .33), quad(3, '0415', 0, cs * .33),
    quad(3, '2637', 0, cs * .33), quad(2, '0143', 1, cs * .55), quad(2, '0253', 1, cs * .55), quad(2, '1245', 1, cs * .55));
  // Rays from frame 64; in the burst, a shockwave ring and the rays thrown
  // out to the edges.
  if (e > 0) {
    const R = 6 + 150 * ee, nr = M.max(8, k.r);
    d.push([4, [120, 67, R, R + 3 + 9 * (1 - e), t * .02, nr, 2 * M.PI / nr, (e < .3 ? OR : WH) ^ iv]]);
    const nb = rnd(k.r * (1 - e));
    if (nb) d.push([4, [120, 67, 10 + 260 * ee, 40 + 300 * ee, g * .07, nb, 2 * M.PI / nb, OR ^ iv]]);
  } else {
    const nr = rnd(k.r * smooth(ramp(g, 64, 96)) * (1 - im) * (1 - .6 * smooth(ramp(t, 78, 88))));
    for (let j = 0; j < k.d && nr; ++j)
      d.push([4, [120, 67, cs * .4, 30 + 120 * crit, g * .07 + j * M.PI / nr, nr, 2 * M.PI / nr, (fl ? OR : WH) ^ iv]]);
  }
  // Dust falls into the core on the real clock (stop-motion is the shell's,
  // not dropped frames; the rays thin out to show it): 8 rings of streaks, r = sqrt of the fall so it
  // speeds up near the core, swirling as it goes in; held in the stop,
  // crushed in the fall.
  const np = rnd(k.r / 4 * smooth(ramp(t, 76, 86)) * (1 - im)), tp = t < 100 ? t : 100;
  for (let i = 0; i < 8 && np && t < 115; ++i) {
    const u = (i / 8 - tp / 36) % 1 + 1, f = M.sqrt(u % 1), r = (24 + 116 * f) * (1 - im);
    d.push([4, [120, 67, r, r + 4 + 12 * (1 - f), i * 1.7 + 30 / (r + 9), np, 2 * M.PI / np, (i & 1 ? 0x07ff : 0xfd20) ^ iv]]);
  }
  // Stars stretch outwards in the dolly zoom and rush past in the burst;
  // the noise is still in the stop and spends itself in the burst.
  const gs = 1 + .8 * dz * (1 - im) + 1.4 * ee, nn = rnd(k.n * (t < 100 ? crit ** 1.5 : t < 115 ? 0 : 1 - e));
  if (t < 132) d.push([5, [190 * gs, 0, 0, 70 * gs, g * .05 + 2 * ee, 120, 67]]);
  if (nn) d.push([6, [t * 1.7, nn, 10 + 40 * crit]]);
  // Backdrop: grey flashes, dark grey on the steps, light grey negatives,
  // and the burst's glow fading back to black.
  const glow = rnd(9 * (1 - ramp(t, 117, 129)) ** 2);
  return {b: neg ? 0xad55 : fl ? 0x5acc : step ? 0x2945 : t >= 117 && glow ? glow << 11 | glow << 6 | glow : 0, s: 0, d: d};
}
const RC = [28, 108];
const A2 = {
  names: ['TWIST', 'ZENITH', 'CORE', 'LOST'], len: [128, 96, 140, 64],
  // Plan list of scene i: [template, args...]; sh carries points.
  plans: function (i, tier) {
    const k = KN[tier], z = k.z;
    if (!i) return corrList(k.c).concat([['st', z.st]]);
    if (i === 2) return [['tri'], ['qd', GR], ['qd', OR], ['sg', GR], ['ry'], ['st', z.st], ['nz', k.k.n]];
    if (i === 3) return [['ln', WH], ['ln', BL]];
    const p = [['st', z.st], ['gr'], ['la', z.ln], ['ci', z.cr, z.cc, 3.2 / z.cc], ['sa', z.sa],
      ['tr', z.sa, z.ts]];
    for (let b = 0; b < 8; ++b) p.push(['sh', b]);
    return p.concat([['rd', z.rr], ['sw', z.sa, RC[0], RC[1]]]);
  },
  // {b: backdrop, s: surface, d: [[plan, inputs]...]}
  frame: function (i, tier, t) {
    const k = KN[tier], z = k.z;
    if (!i) {
      // The wire opens: stars burst from the vanishing point while the
      // corridor thins to its frames and arches, then to nothing; at the cut
      // the field is ZENITH's own (g = 1).
      const e = (t - 88) / 40, g = e * e;
      const d = corrDraws(k.c, t, 128, 1 / 6).filter(q => t < 108 || t < 120 && q[0] % 2 === 0);
      if (e > 0) d.push([7, [190 * g, 0, 0, 70 * g, (t - 128) * .05, 120, 67]]);
      return {b: 0, s: 0, d: d};
    }
    if (i === 2) return core(k.k, t);
    if (i === 3) {
      // CRT off: the line shrinks to a dot, then the dot goes.
      const w = t < 10 ? 2 + 118 * (1 - t / 10) ** 2 : 2.5 - (t - 10) / 4;
      return {b: 0, s: 0, d: w > 0 ? [[0, [120 - w, 67, 120 + w, 67]],
        [1, [120 - w * .8, 66, 120 + w * .8, 66]], [1, [120 - w * .8, 68, 120 + w * .8, 68]]] : []};
    }
    const f = flight(t), c = f.c, s = f.s, H = f.H;
    const x = 34 + 24 * M.sin(t * .09) - 120, y = 28 + 10 * M.cos(t * .05) - 67;
    const P = [120 + x * c - y * s, 67 + x * s + y * c], st = [46 * c - 6 * s, 46 * s + 6 * c];
    const u = [c, s], b = M.max(0, M.min(7, rnd(-M.atan2(s, c) / .14 + 3.5)));
    const d = [[0, [190 * c, 190 * s, -70 * s, 70 * c, t * .05, 120, 67]],
      [1, H.concat([260 * c, 260 * s, 2.2 * M.pow(1.32, (t * .25) % 1), z.gl])],
      [2, H.concat(u, [-(z.ln - 1) * .11 - ((t * .04) % 1) * .22, 150])],
      [3, H.concat(u, [4.5, M.min(1, t / 70) * 2.2])],
      [4, P.concat(st, [10 * M.cos(t * .3), 10 * M.sin(t * .3), .35, 10])]];
    if (t % 16 < 5) d.push([5, [120, 100].concat(P, st, [20, -40])]);
    d.push([6 + b, [120, 120, 120, 123 + (t % 3) * 2]],
      // The radar: rings, a sweep and the formation's blips, in the corner.
      [14, [RC[0], RC[1], 22 / 1.45 ** (z.rr - 1)]], [15, [RC[0] + 22 * M.cos(t * .3), RC[1] + 22 * M.sin(t * .3),
        RC[0] + (P[0] - 120) * .15, RC[1] + (P[1] - 67) * .15, st[0] * .15, st[1] * .15]]);
    return {b: 0, s: 0, d: d};
  }
};

// LEN: distinct frames; PLAY: frames on screen (each news beat twice).
const NAMES = ['NEWS', 'NEWS', 'NEWS'].concat(A2.names), LEN = [16, 16, 16].concat(A2.len),
  PLAY = LEN.map((n, s) => s < 3 ? 2 * n : n);
// One thunk per plan: the loader builds one program array at a time.
function specs(s, tier) {
  return (s > 2 ? A2.plans(s - 3, tier) : [0, 1, 2, 3, 4]).map(e => () => s < 3 ?
    [program(s, e), pointBatch(s, e)] :
    [prog(T[e[0]], e.slice(1)), e[0] === 'sh' ? points(e[1]) : null]);
}
function frameOf(s, tier, t) {
  if (s > 2) return A2.frame(s - 3, tier, t);
  const d = [];
  for (let l = 0; l < 5; ++l) d.push([l, inputs(s * 16 + t, l)]);
  return {b: backdrops[s], s: 0, d: d};
}

globalThis.procMegademo = {
  program: program,
  inputs: inputs,
  pointBatch: pointBatch,
  backdrop: function (frame) {
      return backdrops[Math.floor(frame / 16)];
  },
  names: NAMES, len: LEN, specs: specs, frame: frameOf, tiers: KN.length
};
