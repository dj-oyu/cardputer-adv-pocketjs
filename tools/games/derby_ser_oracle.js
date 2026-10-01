// Host only (test_derby_host.c with DERBY_SER_ORACLE=1; docs/apps/derby-ser-native.md
// section 8). The JS ser(), ou(), lim(), inr() and pan()'s VE/VC of vm/main
// fac3552 (apps/derby/derby_pan.js), the specification main/pocket/pocket_derby.c
// translates. Each pocket.derby.ser() call runs this first; its draws (handle
// and the eight doubles) are compared with the ones the C passed to draw(),
// bit for bit (__sercmp). __serfuzz(n) compares n random cameras, courses and
// series, the draws not run (__serdry). Changes from the original are marked
// "//*": dr() records instead of drawing; the plan comes as a handle (turf: b
// omitted); zf, t, the stands' plans and constants come from view()'s
// arguments, which outside the fuzz must equal the app's own values.
(function () {
  const S = pocket.derby, NV = S.view, NS = S.ser, NC = S.course;
  let VE, VC, Lo, Hi, ZF, T, HL, CR, HC, CE, rec, fuzz = 0;
  const dr = (h, a) => { if (h) rec.push(h, ...a); }; //*
  function lim(c0, c1) { if (c1 > 0) Lo = mx(Lo, -c0 / c1); else if (c1 < 0) Hi = mn(Hi, -c0 / c1); else if (c0 < 0) Hi = -1e9; }
  function ser(h, turf, w, s, g0, L, zf, a, b) { //*
    let P, pg;
    for (let i = 0; i < VC.length; ++i) {
      const j = VC[i];
      if (VE && ou(j, w, s, zf)) continue;
      const ga = g0 + rnd((CH[j] - g0) / s) * s, gb = g0 + rnd((CH[j + 1] - g0) / s) * s, A = ga === pg ? P : pose(ga, w), B = P = pose(pg = gb, w),
        dx = (B[0] - A[0]) / (gb - ga), dz = (B[1] - A[1]) / (gb - ga), X = A[0] + (g0 - ga) * dx - pc[0], Y = A[1] + (g0 - ga) * dz - pc[1],
        Q = (X * pc[2] + Y * pc[3]) / pc[4], q = [X * pc[3] - Y * pc[2] + 120 * Q, Q], u = (dx * pc[2] + dz * pc[3]) / pc[4], v = dx * pc[3] - dz * pc[2] + 120 * u;
      Lo = ga - g0; Hi = gb - g0;
      lim(q[1] - .02, u); lim(zf / pc[4] - q[1], -u); lim(q[0] + 40 * q[1], v + 40 * u); lim(280 * q[1] - q[0], 280 * u - v);
      if (!(Lo < Hi)) continue;
      let c0 = a, c1 = b;
      if (turf) { //*
        const l = a / M.sqrt(dx * dx + dz * dz), x = -dz * l, z = dx * l;
        c1 = -(x * pc[2] + z * pc[3]) / pc[4]; c0 = x * pc[3] - z * pc[2] - 120 * c1;
      }
      const o = u < 0 ? -1 : 1, U = u * o, V = v * o, t1 = o > 0 ? Hi : -Lo;
      let k = s, Z = M.sqrt(M.abs(v * q[1] - q[0] * u) * s / L), n0 = flo((o > 0 ? Lo : -Hi) / s) * s, e;
      while (Z < q[1] + t1 * U && 2 * k * U < .3 * (q[1] + t1 * U) && k < 64 * s) k *= 2, Z *= M.SQRT2;
      e = M.ceil(t1 / k) * k;
      if (!inr(q, U, V, e)) e -= k;
      if (!inr(q, U, V, n0)) n0 += s;
      if (!L && (rnd(o * e / s) + T) & 1) e -= s; //* T: view()'s t
      for (;;) {
        Z /= M.SQRT2;
        const h2 = k > s ? M.ceil(mx(U > 1e-7 ? (mx(Z, k * U / .3) - q[1]) / U : -1e9, n0) / k) * k : n0, c = rnd((e - h2) / k), z = q[1] + e * U; //* h2
        if (c > 0) dr(h, [q[0] + e * V, -V * k, -z, U * k, mn(255, c), 1 / z, L ? c0 : -e / s * 2.39996 % (2 * PI) - 2 * PI * rnd(c * .191) + 1.8, c1]);
        if (k === s) break;
        e = mn(e, h2); k /= 2;
      }
      if (b < 0) {
        const y = q[1] + Lo * u, z = q[1] + Hi * u;
        const xa = (q[0] + Lo * v) / y, xb = (q[0] + Hi * v) / z;
        dr(HL, [xa, 1 / y, xb, 1 / z, 6, -2.4, HC, 21130]); //* HL, HC
        dr(CR, [xa, 28 + 4.8 / y, xb, 28 + 4.8 / z, -2.4 / y, -2.4 / z, Lo * CE, Hi * CE]); //* CR, CE
      }
    }
  }
  function ou(j, w, m, zf) {
    const a = 4 * j, f = pc[4], k = m * M.sqrt(f * f + 25600), l = VE[a] + w * VE[a + 2], d = VE[a + 1] + w * VE[a + 3],
      L = VE[a + 4] + w * VE[a + 6], D = VE[a + 5] + w * VE[a + 7];
    return d < .02 * f - m && D < .02 * f - m || d > zf + m && D > zf + m || 160 * d + l * f < -k && 160 * D + L * f < -k ||
      160 * d - l * f < -k && 160 * D - L * f < -k;
  }
  function inr(q, U, V, e) {
    const z = q[1] + e * U, x = (q[0] + e * V) / z;
    return z > .02 && x > -400 && x < 640;
  }
  // pan()'s chord ends and chords in view, for pc and zf.
  function chords(zf) {
    VE = CRS === OC && []; VC = VE ? [] : [0];
    if (VE) {
      for (const g of CH) {
        const m = pose(g, 0), x = m[0] - pc[0], z = m[1] - pc[1];
        VE.push(x * pc[3] - z * pc[2], x * pc[2] + z * pc[3], -m[3] * pc[3] - m[2] * pc[2], m[2] * pc[3] - m[3] * pc[2]);
      }
      for (let j = 1; j < CH.length; ++j) if (!ou(j - 1, 25.5, 30, zf)) VC.push(j - 1);
    }
  }
  S.view = function (p, zf, t2, hl, cr, hc, ce) {
    if (!fuzz && (p !== pc || t2 !== t || hl !== live.hl || cr !== live.crowd || hc !== KN[tier][0] || ce !== KN[3][5]))
      throw Error('SER_ORACLE view() arguments are not the app\'s');
    ZF = zf; T = t2; HL = hl; CR = cr; HC = hc; CE = ce;
    chords(zf);
    return NV.apply(S, arguments);
  };
  S.ser = function (h, w, s, g0, L, a, b) {
    rec = [];
    ser(h, b === undefined, w, s, g0, L, ZF, a, b);
    __serbegin();
    const r = NS.apply(S, arguments);
    __sercmp(rec, fuzz);
    return r;
  };
  // n random cases, each a course, a camera and the five series of pan() at a
  // random tier plus three random ones; the app's course, camera and tier are
  // put back afterwards.
  globalThis.__serfuzz = function (n, seed) {
    const r = rng(seed), sv = [CRS, CH, pc, tier], pick = l => l[flo(r() * l.length)];
    fuzz = 1; __serdry(1);
    try {
      for (let c = 0; c < n; ++c) {
        const oval = r() < .7;
        CRS = oval ? OC : SC; CH = [-1e4];
        if (oval) for (let j = 0; j <= PAN[3]; ++j) CH.push(OB + j * 120 * PI / PAN[3]);
        CH.push(1e4);
        S.course(CH, oval ? OC : 0, DNR);
        tier = flo(r() * 3);
        const k = KN[tier], g = -150 + 1300 * r(), gc = pick([g, g - 40 + 80 * r(), 100 + 900 * r(), 460, 820]),
          wc = pick([-14, -100, -3, -150 + 210 * r(), 11 + 30 * r()]), q = pose(gc, wc), a = pose(g, (DNR + DFR) / 2),
          x = a[0] - q[0] + (r() < .1 ? 1e-9 : 0), z = a[1] - q[1], e = M.sqrt(x * x + z * z) || 1;
        pc = [q[0], q[1], x / e, z / e, pick([200, 1500, mn(1500, mx(200, 14 * e / 2.4)), 50 + 3000 * r()]), e];
        const zf = pc[5] + pick([75, 150, 500 * r()]), L = pick([4, 8, 4, 8, 0]), t2 = flo(r() * 1000);
        S.view(pc, zf, t2, pick([7, 0]), pick([9, 0]), k[0], KN[3][5]);
        S.ser(3, 40, 12, 0, L, 31727, -7.5);
        S.ser(3, DFR, k[4], 0, L, 0xad55, 4.9);
        S.ser(4, DNR, 2 * k[5], 0, L, 11.6);
        S.ser(5, DNR, 2 * k[5], k[5], L, 11.6);
        S.ser(3, DNR, k[4], 0, L, 0xffff, 4.9);
        for (let i = 0; i < 3; ++i) {
          const s = pick([1, 2.5, 4, 5, 7, 8, 10, 12, 14, 16 * r() + .5]), b = pick([-7.5, 4.9, -1 - 9 * r(), 9 * r(), undefined]);
          b === undefined ? S.ser(6, 5 + 45 * r(), s, pick([0, s / 2, s * r()]), L, 5 + 20 * r()) :
            S.ser(6, 5 + 45 * r(), s, pick([0, s / 2, s * r()]), L, pick([31727, 0xad55, 0xffff]), b);
        }
      }
    } finally {
      fuzz = 0; __serdry(0);
      [CRS, CH, pc, tier] = sv;
      S.course(CH, CRS === OC ? OC : 0, DNR);
    }
  };
})();
