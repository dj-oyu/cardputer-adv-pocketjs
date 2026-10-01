// Source patches of DERBY WATCH's panning view for docs/apps/derby-trig-cull.md:
// each variant rewrites derby_view.js / derby_pan.js text (exact substrings, so
// a change in the app fails loudly here). trig_cull.mjs counts them on the
// host; cull_shots.mjs runs them in the C harness for pixels.
export const VARIANTS = {};

// hoist: wide() posed the leader once per unit (3 times a frame, the same g
// and w); once is the same value.
const HOIST = [['view', `  for (let i = 7; i < 10; ++i) {
    const q = pose(CAMS[i][3], CAMS[i][CRS === OC ? 5 : 4]), a = pose(g, (DNR + DFR) / 2), x = a[0] - q[0], z = a[1] - q[1];`,
`  const a = pose(g, (DNR + DFR) / 2);
  for (let i = 7; i < 10; ++i) {
    const q = pose(CAMS[i][3], CAMS[i][CRS === OC ? 5 : 4]), x = a[0] - q[0], z = a[1] - q[1];`]];
VARIANTS.hoist = { edits: HOIST };

// vetab: pan()'s chord ends (CH at w 0) posed once a race into a Float64Array
// (exact: the same doubles pose() returns), read there every frame.
const VETAB = [
  ['view', 'let CRS = SC, CH, VC, VE;', 'let CRS = SC, CH, CP, VC, VE;'],
  ['scene', '    CH.push(1e4);\n', '    CH.push(1e4);\n    CP = new Float64Array(4 * CH.length); CH.forEach((g, i) => CP.set(pose(g, 0), 4 * i));\n'],
  ['pan', `    for (const g of CH) {
      const m = pose(g, 0), x = m[0] - pc[0], z = m[1] - pc[1];
      VE.push(x * pc[3] - z * pc[2], x * pc[2] + z * pc[3], -m[3] * pc[3] - m[2] * pc[2], m[2] * pc[3] - m[3] * pc[2]);
    }`, `    for (let a = 0; a < 4 * CH.length; a += 4) {
      const x = CP[a] - pc[0], z = CP[a + 1] - pc[1], c = CP[a + 2], d = CP[a + 3];
      VE.push(x * pc[3] - z * pc[2], x * pc[2] + z * pc[3], -d * pc[3] - c * pc[2], c * pc[3] - d * pc[2]);
    }`]];
VARIANTS.vetab = { edits: [...HOIST, ...VETAB] };
// fixtab: vetab, and every other point fixed for a race in the same table
// after CH's: the three panning units, the screen's ends, the poles (5 at the
// far rail, the finish's near end). fp(k): entry k as pose() returns it.
VARIANTS.fixtab = { edits: [...HOIST, ...VETAB.map(e => e[0] !== 'scene' ? e : [e[0], e[1], `    CH.push(1e4);
    CP = new Float64Array(4 * CH.length + 44); CH.concat(7, 8, 9, 0, 0, 200, 400, 600, 800, 1000, D).forEach((g, i) => { const k = i - CH.length; CP.set(k < 0 ? pose(g, 0) : k < 3 ? pose(CAMS[g][3], CAMS[g][CRS === OC ? 5 : 4]) : k < 5 ? pose(VS[0] + (2 * k - 7) * VS[2], VS[1]) : pose(g, k < 10 ? DFR : DNR), 4 * i); });
`]),
  ['view', 'function pose(g, w) {', 'const fp = k => [CP[k *= 4], CP[k + 1], CP[k + 2], CP[k + 3]];\nfunction pose(g, w) {'],
  ['view', '    const q = pose(CAMS[i][3], CAMS[i][CRS === OC ? 5 : 4]), x = a[0] - q[0]', '    const q = fp(CH.length + i - 7), x = a[0] - q[0]'],
  ['view', '    const q = pose(c[3], c[CRS === OC ? 5 : 4]), a = pose(', '    const q = fp(CH.length + m - 7), a = pose('],
  ['scene', '  const a = pose(VS[0] - VS[2], VS[1]), b = pose(VS[0] + VS[2], VS[1]);', '  const a = fp(CH.length + 3), b = fp(CH.length + 4);'],
  ['pan', 'const p = pj(pose(m, DFR)), r = 1 / p[1], x = p[0] * r, e = m === D, n = e ? pj(pose(m, DNR)) : p', 'const p = pj(fp(CH.length + 4 + m / 200)), r = 1 / p[1], x = p[0] * r, e = m === D, n = e ? pj(fp(CH.length + 10)) : p'],
] };
// vetabA: the same in a plain Array (the fewest guest bytes of the forms
// lut_forms.mjs measured: a typed array carries a fixed cost of its own).
VARIANTS.vetabA = { edits: [...HOIST, ...VETAB.map(e => e[0] === 'scene' ? [e[0], e[1], '    CH.push(1e4);\n    CP = [].concat.apply([], CH.map(g => pose(g, 0)));\n'] : e)] };
// vetabL: vetabA built by a loop (one pose's array alive at a time, not the
// map's 19 at once: the evaluation's peak).
VARIANTS.vetabL = { edits: [...HOIST, ...VETAB.map(e => e[0] === 'scene' ? [e[0], e[1], '    CH.push(1e4);\n    CP = [];\n    for (const g of CH) CP.push.apply(CP, pose(g, 0));\n'] : e)] };
// vetabH: no new global name (one more top-level binding stepped the
// evaluation's peak +1.5 KB, code_bytes.mjs): the table rides on CH as CH.P.
VARIANTS.vetabH = { edits: [...HOIST,
  ['scene', '    CH.push(1e4);\n', '    CH.push(1e4);\n    CH.P = [];\n    for (const g of CH) CH.P.push.apply(CH.P, pose(g, 0));\n'],
  ['pan', VETAB[2][1], VETAB[2][2].replace('    for (let a', '    const P = CH.P;\n    for (let a').replace(/CP\[/g, 'P[')]] };
// vetabS: the bend's chord ends from 9 sines a race (PAN[3] / 2 + 1), by
// symmetry: end j (1..PAN[3] + 1) heads pi - i pi/n (i = j - 1), so its sin
// is sin(i pi/n) and its cos -cos(i pi/n) = -sin((n/2 - i) pi/n) (the swap),
// mirrored past the middle (sin(pi - x) = sin x, cos(pi - x) = -cos x); the
// point is pose()'s bend formula. The two straight sentinels are posed.
VARIANTS.vetabS = { edits: [...HOIST,
  ['scene', '    CH.push(1e4);\n', '    CH.push(1e4);\n    CH.P = [];\n    for (let k = 0; k <= PAN[3] / 2; ++k) CH.P.push(sin(k * PI / PAN[3]));\n'],
  ['pan', VETAB[2][1], `    const e = OC[1], r = 1 / e[1] + DNR, S = CH.P, n = PAN[3], h = n / 2;
    for (let j = 0; j < CH.length; ++j) {
      const i = j - 1, b = i > h, s = S[b ? n - i : i], C = b ? S[i - h] : -S[h - i],
        m = i < 0 || i > n ? pose(CH[j], 0) : [e[2] + (s - e[5]) * r, e[3] - (C - e[4]) * r, C, s], x = m[0] - pc[0], z = m[1] - pc[1];
      VE.push(x * pc[3] - z * pc[2], x * pc[2] + z * pc[3], -m[3] * pc[3] - m[2] * pc[2], m[2] * pc[3] - m[3] * pc[2]);
    }`]] };
// For code_bytes.mjs only (not runnable): vetabL's edits one chunk at a time.
VARIANTS._view = { edits: [...HOIST, VETAB[0]] };
VARIANTS._scene = { edits: [VETAB[0], VARIANTS.vetabL.edits[2]] };
VARIANTS._pan = { edits: [VETAB[0], VETAB[2]] };
// vetab32: the same in a Float32Array (half the bytes; chord ends to ~1 mm).
VARIANTS.vetab32 = { edits: [...HOIST, ...VETAB.map(e => e[0] === 'scene' ? [e[0], e[1], e[2].replace('Float64Array', 'Float32Array')] : e)] };

// ---- Culling (stage 3). cullT: a chord body whose box on the panel (its
// ground line, and its top at the series' height) is under T px is skipped.
const SER_HEAD = `    if (!(Lo < Hi)) continue;\n`;
for (const T of [1, 2, 3, 4]) VARIANTS['cull' + T] = { edits: [['pan', SER_HEAD, SER_HEAD +
`    { const H = n === 'prail' ? (b < 0 ? 13.5 : 1.1) : n === 'pk' ? 7.2 : 0, za = q[1] + Lo * u, zb = q[1] + Hi * u, xa = (q[0] + Lo * v) / za, xb = (q[0] + Hi * v) / zb;
      if (M.max(M.abs(xb - xa), M.max(6 / za, 6 / zb, (6 - H) / za, (6 - H) / zb) - M.min(6 / za, 6 / zb, (6 - H) / za, (6 - H) / zb)) < ${T}) continue; }\n`]] };

// mergeT: neighbouring chords in view become one body while the chord ends
// skipped lie within T px of the straight line on the panel (ground, and a top
// 13.5 m up), all in front of the unit. VE gives each end's [across, depth]
// at w (pan()).
const SER_LOOP = `  for (let i = 0; i < VC.length; ++i) {
    const j = VC[i];
    if (VE && ou(j, w, s, zf)) continue;
    const ga = g0 + rnd((CH[j] - g0) / s) * s, gb = g0 + rnd((CH[j + 1] - g0) / s) * s,`;
for (const T of [0.5, 1, 2, 4]) VARIANTS['merge' + T] = { edits: [['pan', SER_LOOP, `  for (let i = 0; i < VC.length; ++i) {
    const j = VC[i];
    if (VE && ou(j, w, s, zf)) continue;
    let je = j + 1;
    while (VE && VC[i + 1] === je && dev(j, je, je + 1, w) < ${T}) ++je, ++i;
    const ga = g0 + rnd((CH[j] - g0) / s) * s, gb = g0 + rnd((CH[je] - g0) / s) * s,`],
  ['pan', 'function ou(j, w, m, zf) {', `function dev(a, k, c, w) {
  const f = pc[4], P = i => [VE[4 * i] + w * VE[4 * i + 2], VE[4 * i + 1] + w * VE[4 * i + 3]], A = P(a), K = P(k), C = P(c);
  if (A[1] < .02 * f || K[1] < .02 * f || C[1] < .02 * f) return 1e9;
  let m = 0;
  for (const h of [6, -7.5]) {
    const s = p => [120 + p[0] * f / p[1], 28 + h * f / p[1]], a = s(A), b = s(K), c = s(C), dx = c[0] - a[0], dy = c[1] - a[1];
    m = M.max(m, M.abs((b[0] - a[0]) * dy - (b[1] - a[1]) * dx) / (M.sqrt(dx * dx + dy * dy) || 1));
  }
  return m;
}
function ou(j, w, m, zf) {`]] };

// mergeFT: the same, decided once a frame in pan() (not per series) for the
// inner rail and the stands' depth (w 11 and 40), into VM ([first chord, end
// index] pairs); ser() and ou() take the runs. dv(a, c): the largest panel
// distance of the chord ends between a and c from the line a-c, at the top of
// the stands (7.5 m over the units' 6 m eye: the farthest from the horizon of
// anything drawn on the chords; on the panel the offset is h f |X| /
// sqrt(du^2 + h^2 dv^2) for an end at height h, X the cross product in (u,
// v) = (across / depth, 1 / depth), so the top bounds the ground).
const MERGEF = T => [
  ['pan', `'use strict';\n`, `'use strict';\nlet VM;\n`],
  ['pan', `    for (let j = 1; j < CH.length; ++j) if (!ou(j - 1, 25.5, 30, zf)) VC.push(j - 1);
  }`, `    for (let j = 1; j < CH.length; ++j) if (!ou(j - 1, 25.5, 30, zf)) VC.push(j - 1);
  }
  VM = [];
  for (let i = 0; i < VC.length; ++i) {
    const j = VC[i];
    let e = j + 1;
    while (VE && VC[i + 1] === e && dv(j, e + 1) < ${T}) ++e, ++i;
    VM.push(j, e);
  }`],
  ['pan', SER_LOOP, `  for (let i = 0; i < VM.length; i += 2) {
    const j = VM[i], je = VM[i + 1];
    if (VE && ou(j, w, s, zf, je)) continue;
    const ga = g0 + rnd((CH[j] - g0) / s) * s, gb = g0 + rnd((CH[je] - g0) / s) * s,`],
  ['pan', `function ou(j, w, m, zf) {
  const a = 4 * j, f = pc[4], k = m * M.sqrt(f * f + 25600), l = VE[a] + w * VE[a + 2], d = VE[a + 1] + w * VE[a + 3],
    L = VE[a + 4] + w * VE[a + 6], D = VE[a + 5] + w * VE[a + 7];`, `function dv(a, c) {
  let m = 0;
  for (let w = 11; w < 41; w += 29) {
    const A = 4 * a, C = 4 * c, da = VE[A + 1] + w * VE[A + 3], dc = VE[C + 1] + w * VE[C + 3];
    if (da < .02 * pc[4] || dc < .02 * pc[4]) return 1e9;
    const ua = (VE[A] + w * VE[A + 2]) / da, va = 1 / da, du = (VE[C] + w * VE[C + 2]) / dc - ua, dd = 1 / dc - va, n = 7.5 * pc[4] / M.sqrt(du * du + 56.25 * dd * dd);
    for (let k = a + 1; k < c; ++k) {
      const K = 4 * k, d = VE[K + 1] + w * VE[K + 3];
      if (d < .02 * pc[4]) return 1e9;
      m = mx(m, M.abs(((VE[K] + w * VE[K + 2]) / d - ua) * dd - (1 / d - va) * du) * n);
    }
  }
  return m;
}
function ou(j, w, m, zf, e = j + 1) {
  const a = 4 * j, b = 4 * e, f = pc[4], k = m * M.sqrt(f * f + 25600), l = VE[a] + w * VE[a + 2], d = VE[a + 1] + w * VE[a + 3],
    L = VE[b] + w * VE[b + 2], D = VE[b + 1] + w * VE[b + 3];`]];
for (const T of [0.5, 1, 2]) VARIANTS['mergeF' + T] = { edits: MERGEF(T) };
VARIANTS['vetab+mergeF1'] = { edits: [...VARIANTS.vetab.edits, ...MERGEF(1)] };

function rep(s, a, b) {
  if (!s.includes(a)) throw new Error('variants.mjs: substring not found: ' + a.slice(0, 80));
  return s.replace(a, b);
}

export function patchApp(files, v) {
  const f = { ...files };
  if (!v) return f;
  for (const [file, a, b] of VARIANTS[v].edits) f[file] = rep(f[file], a, b);
  return f;
}
