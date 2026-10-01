// Host numbers behind docs/kasane/oval-pie-design.md section 7: nothing here
// runs on the device. float32 is modelled with Math.fround after every
// operation (the procedural VM's registers are float32).
//
//   node tools/games/ovalcost/oval_math.mjs
const f = Math.fround, PI = Math.PI;
const row = (...c) => console.log('| ' + c.join(' | ') + ' |');
const e = (x, d = 2) => x === 0 ? '0' : Math.abs(x) >= .01 && Math.abs(x) < 1e4 ? x.toFixed(d + 1) : x.toExponential(d);

// ---- F1. A reciprocal from the neighbour's: the error after k steps of
// r <- r(2 - Zr) started at 1/Z_prev is d^(2^k), d = |Z - Z_prev| / Z_prev.
// The screen error is at most 160 px (the edge, with the 40 px guard) times it.
console.log('\nF1. Newton from the previous point: largest depth step d for a screen error (edge = 160 px from the centre)');
row('method', 'VM steps', 'error', 'd for 0.1 px', 'd for 0.5 px');
const solve = (g, tol) => { let lo = 0, hi = .99; for (let i = 0; i < 60; ++i) { const m = (lo + hi) / 2; if (160 * g(m) < tol) lo = m; else hi = m; } return lo; };
for (const [name, st, g] of [
  ['Newton x1', 3, d => d ** 2], ['Newton x2', 6, d => d ** 4], ['Newton x3 (now)', 9, d => d ** 8],
  ['linear extrapolation + Newton x1', 6, d => (2 * d * d / (1 - d)) ** 2],
  ['cubic step r(1+e+e^2) x1', 5, d => d ** 3], ['quartic step r(1+e)(1+e^2) x1', 6, d => d ** 4]])
  row(name, st, name.includes('extra') ? '4d^4/(1-d)^2' : '', solve(g, .1).toFixed(3), solve(g, .5).toFixed(3));
// The same in float32 on a line, far to near, as the plans do it.
{
  console.log('\nF1b. float32 walk, 200 posts, depth 300 m -> near, X = 0.4 Z + 20 (px error at f = 200, points within 160 px of the centre)');
  row('depth step d at the near end', 'Newton x1', 'x2', 'x3', 'exact divide (float32)');
  for (const dn of [.05, .1, .2, .3]) {
    const out = [];
    for (const k of [1, 2, 3, 0]) {
      // Z_i geometric so that the step ratio is dn everywhere (the worst case of a bound on d).
      let Z = 300, r = f(1 / Z), worst = 0;
      for (let i = 0; i < 200 && Z > 3; ++i) {
        Z = Z * (1 - dn);
        const z32 = f(Z);
        if (k) for (let j = 0; j < k; ++j) r = f(r * f(2 - f(z32 * r))); else r = f(1 / z32);
        const X = f(.4 * Z + 20), x = f(200 * f(X * r)), ex = 200 * (.4 * Z + 20) / Z;
        if (Math.abs(ex) < 160) worst = Math.max(worst, Math.abs(x - ex));
      }
      out.push(e(worst));
    }
    row(dn, ...out);
  }
}

// ---- F2. Walking an arc: rotation recurrence against Chebyshev's, float32.
// Error is the distance (m) from the exact point of radius rho, in px at
// 8 px per m (f 200 at 25 m), the nearest a rail comes to a panning unit.
function walk(kind, rho, dth, n, reseed) {
  const c = f(Math.cos(dth)), s = f(Math.sin(dth)), k2 = f(2 * c);
  let u = f(rho), v = f(0), pu = f(rho * Math.cos(-dth)), pv = f(rho * Math.sin(-dth)), worst = 0;
  for (let i = 1; i <= n; ++i) {
    if (kind === 'rot') { const nu = f(f(u * c) - f(v * s)), nv = f(f(v * c) + f(u * s)); u = nu; v = nv; }
    else { const nu = f(f(k2 * u) - pu), nv = f(f(k2 * v) - pv); pu = u; pv = v; u = nu; v = nv; }
    if (reseed && i % reseed === 0) { u = f(rho * Math.cos(i * dth)); v = f(rho * Math.sin(i * dth)); pu = f(rho * Math.cos((i - 1) * dth)); pv = f(rho * Math.sin((i - 1) * dth)); }
    worst = Math.max(worst, Math.hypot(u - rho * Math.cos(i * dth), v - rho * Math.sin(i * dth)));
  }
  return worst;
}
console.log('\nF2. arc walk in float32: largest position error, m (px at 8 px/m)');
row('radius, step', 'method', '100 steps', '400', '1000', '1000, reseeded every 64');
for (const [rho, ds, note] of [[131.6, 5, 'far rail, posts 5 m'], [149, 3, 'stands, crowd column 3 m'], [149, .75, 'HEAVY-like 0.75 m'], [1e4, 5, 'a straight as R = 10 km'], [1e5, 5, 'a straight as R = 100 km']]) {
  const dth = ds / (rho > 1e3 ? rho : 120);
  for (const kind of ['rot', 'cheb'])
    row(`${rho} m, ${ds} m (${note})`, kind === 'rot' ? 'rotation' : 'Chebyshev', ...[100, 400, 1000].map(n => { const w = walk(kind, rho, dth, n, 0); return `${e(w)} (${e(w * 8)})`; }),
      (() => { const w = walk(kind, rho, dth, 1000, 64); return `${e(w)} (${e(w * 8)})`; })());
}

// ---- F3. The rational quadratic Bezier of a circular arc under perspective.
{
  console.log('\nF3a. projecting the control points (weights x depth) against projecting the arc: largest |dx| px, 101 samples');
  const R = 131.6, cam = [95, 0], dir = [Math.cos(1.9), Math.sin(1.9)], F = 300;
  const proj = P => { const a = P[0] - cam[0], b = P[1] - cam[1], Z = a * dir[0] + b * dir[1]; return [F * (a * dir[1] - b * dir[0]) / Z, Z]; };
  row('arc (deg)', 'max |dx| px (double)', 'uniform-t against uniform-angle: max offset m', 'the same, px at 8 px/m');
  for (const deg of [11.25, 22.5, 45, 90]) {
    const al = deg * PI / 360, th0 = 1.4, w1 = Math.cos(al);
    const P = [[R * Math.cos(th0 - al), R * Math.sin(th0 - al)], [R / w1 * Math.cos(th0), R / w1 * Math.sin(th0)], [R * Math.cos(th0 + al), R * Math.sin(th0 + al)]];
    const S = P.map(proj), W = [S[0][1], w1 * S[1][1], S[2][1]];
    let worst = 0, off = 0;
    for (let i = 0; i <= 100; ++i) {
      const t = i / 100, b = [(1 - t) ** 2, 2 * t * (1 - t), t * t], ww = [1, w1, 1];
      const den = b[0] + b[1] * w1 + b[2], wx = (b[0] * P[0][0] + b[1] * w1 * P[1][0] + b[2] * P[2][0]) / den, wz = (b[0] * P[0][1] + b[1] * w1 * P[1][1] + b[2] * P[2][1]) / den;
      const sx = (b[0] * W[0] * S[0][0] + b[1] * W[1] * S[1][0] + b[2] * W[2] * S[2][0]) / (b[0] * W[0] + b[1] * W[1] + b[2] * W[2]);
      worst = Math.max(worst, Math.abs(sx - proj([wx, wz])[0]));
      const th = th0 - al + 2 * al * t;
      off = Math.max(off, Math.hypot(wx - R * Math.cos(th), wz - R * Math.sin(th)));
    }
    row(deg, e(worst), e(off), e(off * 8));
  }
}

// ---- F4. pose() with the sections' starts stored (two trig calls on a bend,
// none on a straight) against the branch's pose().
{
  const DNR = 11, OB = 650 - 120 * PI, OC = [[650 + OB, -218, PI], [OB, 0], [120 * PI, -1 / 120], [2e3, 0]];
  let trig = 0;
  const sin = x => (++trig, Math.sin(x)), cos = x => (++trig, Math.cos(x));
  function pose(g, w) {
    let x = OC[0][0], z = OC[0][1], a = OC[0][2];
    for (let i = 1; i < OC.length; ++i) {
      const q = OC[i], s = Math.min(g, q[0]), b = a + q[1] * s, r = 1 / q[1] + DNR;
      if (q[1]) x += (sin(b) - sin(a)) * r, z -= (cos(b) - cos(a)) * r; else x += s * cos(a), z += s * sin(a);
      a = b; g -= s;
      if (g <= 0) break;
    }
    x += g * cos(a); z += g * sin(a);
    return [x - w * sin(a), z + w * cos(a), cos(a), sin(a)];
  }
  // Starts: [g0, x, z, heading, cos, sin, curvature] a section, built once a race.
  const ST = [];
  { let x = OC[0][0], z = OC[0][1], a = OC[0][2], g = 0;
    for (let i = 1; i < OC.length; ++i) { const q = OC[i]; ST.push([g, x, z, a, Math.cos(a), Math.sin(a), q[1]]); const p = pose(g + q[0], 0); x = p[0]; z = p[1]; a += q[1] * q[0]; g += q[0]; } }
  function pose2(g, w) {
    let i = ST.length - 1;
    while (i && g < ST[i][0]) --i;
    const S = ST[i], s = g - S[0];
    if (!S[6]) return [S[1] + s * S[4] - w * S[5], S[2] + s * S[5] + w * S[4], S[4], S[5]];
    const b = S[3] + S[6] * s, cb = cos(b), sb = sin(b), r = 1 / S[6] + DNR;
    return [S[1] + (sb - S[5]) * r - w * sb, S[2] - (cb - S[4]) * r + w * cb, cb, sb];
  }
  let worst = 0, n = 0; trig = 0;
  for (let g = 0; g <= 1000; g += .37) for (const w of [-14, 11, 22.6, 40]) { const a = pose(g, w); ++n; }
  const t1 = trig; trig = 0;
  for (let g = 0; g <= 1000; g += .37) for (const w of [-14, 11, 22.6, 40]) { pose2(g, w); }
  const t2 = trig;
  for (let g = 0; g <= 1000; g += .37) for (const w of [-14, 11, 22.6, 40]) { const a = pose(g, w), b = pose2(g, w); for (let k = 0; k < 4; ++k) worst = Math.max(worst, Math.abs(a[k] - b[k])); }
  console.log(`\nF4. pose with stored section starts: ${n} poses, trig per pose ${(t1 / n).toFixed(2)} -> ${(t2 / n).toFixed(2)}, largest difference ${e(worst)} (m or unit vector)`);
  console.log(`    Int16 table of chord ends at 1/32 m: half a step 1/64 m = ${e(8 / 64)} px at 8 px/m; a 64-entry sine table for the crowd's 1.5 px sway: ${e(1.5 * Math.sin(PI / 64))} px`);
}

// ---- F5. An arc plan end to end in float32: centre + rotating radius, one
// reciprocal a point, against the exact projection in double. The camera is
// 25 m inside the inner rail (radius 95), aimed at a leader on the bend.
{
  console.log('\nF5. arc walk + projection in float32 (posts every 5 m of rail, the whole 180 deg bend = 75 steps): largest |dx|, |dy| px over the points in view; every point deeper than 2 m is walked, also off screen, where a Newton chain meets depth ratios over 2 and diverges');
  row('radius', 'reciprocal', 'aim 20 deg ahead', '60 deg', '120 deg (f up to 1500)');
  const camR = 95;
  for (const rho of [120, 131.6, 149]) for (const mode of ['divide', 'Newton x3', 'Newton x2']) {
    const out = [];
    for (const aim of [20, 60, 120]) {
      const ca = aim * PI / 180, L = [115 * Math.cos(ca) - camR, 115 * Math.sin(ca)], dist = Math.hypot(L[0], L[1]), d = [L[0] / dist, L[1] / dist];
      const F = Math.min(1500, Math.max(200, 14 * dist / 2.4)), dth = 5 / 120, n = 75;
      // Centre in camera terms, and the radius vector's (depth, lateral), walked far to near where Newton needs it: here simply in angle order.
      const Zc = f(-camR * d[0]), Xc = f(-camR * d[1] * -1);
      const th0 = -.2;
      let U = f(rho * (Math.cos(th0) * d[0] + Math.sin(th0) * d[1])), V = f(rho * (Math.cos(th0) * d[1] - Math.sin(th0) * d[0]));
      const c = f(Math.cos(dth)), s = f(Math.sin(dth));
      let r = 0, wx = 0, wy = 0, first = true;
      for (let i = 0; i <= n; ++i) {
        const th = th0 + i * dth, Z = f(Zc + U), X = f(f(-camR * d[1]) + V);
        const eZ = (rho * Math.cos(th) - camR) * d[0] + rho * Math.sin(th) * d[1], eX = (rho * Math.cos(th) - camR) * d[1] - rho * Math.sin(th) * d[0];
        if (eZ > 2) {
          if (mode === 'divide' || first) r = f(1 / Z); else for (let j = 0; j < (mode === 'Newton x3' ? 3 : 2); ++j) r = f(r * f(2 - f(Z * r)));
          first = false;
          const x = f(F * f(X * r)), y = f(F * f(6 * r)), ex = F * eX / eZ, ey = F * 6 / eZ;
          if (Math.abs(ex) < 160) { wx = Math.max(wx, Math.abs(x - ex)); wy = Math.max(wy, Math.abs(y - ey)); }
        } else first = true;
        const nu = f(f(U * c) + f(V * s)), nv = f(f(V * c) - f(U * s)); U = nu; V = nv;
      }
      out.push(`${e(wx)}, ${e(wy)}`);
    }
    row(rho, mode, ...out);
  }
}

// ---- F6. A reciprocal for PIE's 16-bit lanes (esp32s3-hw-mcp ex13's form: a
// table indexed by w >> 8) and what one Newton step in integers adds.
{
  console.log('\nF6. integer reciprocal for PIE lanes, w = depth in 1/160 m (2560..65535 = 16..410 m): largest relative error, and px at the screen edge (x 160)');
  row('method', 'relative error', 'px at the edge');
  const rel = fn => { let w0 = 0; for (let w = 2560; w < 65536; w += 7) w0 = Math.max(w0, Math.abs(fn(w) * w - 1)); return w0; };
  const tab = (bits, sh) => w => Math.round(2 ** bits / (((w >> sh) << sh) + (1 << sh) / 2)) / 2 ** bits;
  const newton = (g, bits) => w => { const r = Math.round(g(w) * 2 ** bits), t = 2 * 2 ** bits - Math.floor(w * r / 1), r2 = Math.floor(r * t / 2 ** bits); return r2 / 2 ** bits; };
  for (const [name, fn] of [['256-entry table, Q15 entry x 256 (ex13)', tab(23, 8)], ['1024-entry table (w >> 6), 16-bit entry', tab(23, 6)],
    ['256-entry table + Newton x1 in 32-bit lanes (Q30)', newton(tab(23, 8), 30)], ['256-entry table + Newton x2 in 32-bit lanes', newton(newton(tab(23, 8), 30), 30)]]) {
    const r = rel(fn); row(name, e(r), e(r * 160));
  }
}
