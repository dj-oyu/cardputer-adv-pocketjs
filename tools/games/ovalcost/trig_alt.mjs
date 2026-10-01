// Stage 2 of docs/apps/derby-trig-cull.md: ways to spend less on pose() and
// its cos/sin in DERBY WATCH's panning frames, compared on the host. Counts
// (pose calls by where they land, distinct g, table reads) are exact for the
// scripted frames; microseconds are those counts x the device's unit prices
// (trig_cull.mjs P), so they are estimates. Errors are in metres and in px on
// the panel (|offset| / Z', Z' = depth / f, for points in front of the unit).
//
//   node tools/games/ovalcost/trig_alt.mjs [--step 5]
import { runSet, P } from './trig_cull.mjs';

const arg = (k, d) => { const i = process.argv.indexOf(k); return i < 0 ? d : process.argv[i + 1]; };
const STEP = +arg('--step', 5);
const OB = 650 - 120 * Math.PI, DNR = 11;
const range = (a, b, s) => { const r = []; for (let x = a; x <= b + 1e-9; x += s) r.push(x); return r; };
const where = g => g > OB && g <= 650 ? 'bend' : 'ovalStraight';

// Unit costs (us). pose: measured on the device on the bend (135) and on the
// straight course's shortcut (26); a pose on the oval's straight sections is
// not measured: 135 less its two trig calls and the bend's own arithmetic
// (r, the two angles, the offsets: ~12 ops), about 70 (estimate).
const POSE = { bend: P.poseBend, ovalStraight: 70, straight: P.poseStraight };
// A VE entry read from the table instead of posed (4 reads, 3 index adds, the
// loop's bound CH.length x 4): about 22 us against the for..of step and the 4
// reads of m (about 16) around pose(): the saving is pose - 6 (estimate).
const VE_READ_EXTRA = 6;
// vetabS: a bend end rebuilt from the sines (2 reads, ~6 add, 2 mul, a compare:
// 27 us, lut_forms.mjs ptabSym) and its 4-array (14) against the 16 around pose().
const VE_SYM_EXTRA = 25;
// fp(k): call, k*4, 3 index adds, 4 reads, a 4-array, CH.length + i - 7.
const FP = P.call + P.mul + 3 * P.add + 4 * P.rd + P.arr4 + P.prop + 2 * P.add;
// (b) a per-frame Map keyed by g: get on every bend pose, set on a miss.
// Map.get/set are not measured on the device; a call and a property read each
// (estimate).
const MAP = P.call + P.prop;
// (c) table + linear interpolation, per bend pose instead of 2 trig calls:
// index mul and |0, the fraction, 4 reads, 2 lerps (2 mul, 4 add).
const LERP = 3 * P.mul + 8 * P.add + 4 * P.rd;
// (d) the rotation recurrence (4 mul, 2 add) instead of 2 trig calls, for
// points a fixed angle apart: only VE's chord ends are (CH, pi/16 apart).
const REC = 4 * P.mul + 2 * P.add;
const TRIG2 = 2 * P.trig;

function poseCost(frames, oval) {
  let us = 0, n = 0;
  for (const f of frames) for (const p of f.poses) { us += oval ? POSE[where(p[1])] : POSE.straight; ++n; }
  return { us: us / frames.length, n: n / frames.length };
}

// Error of a cos/sin table of N intervals over the bend's headings [0, pi],
// linearly interpolated (f32: entries rounded to float32), against pose():
// every bend pose of the frames at its own w, VE's chord ends at w 11, 22.6, 40.
function lerpError(frames, N, f32) {
  const R = f32 ? Math.fround : x => x, C = [], S = [];
  for (let i = 0; i <= N; ++i) { C.push(R(Math.cos(i * Math.PI / N))); S.push(R(Math.sin(i * Math.PI / N))); }
  const cs = a => { const t = a * N / Math.PI, i = Math.min(N - 1, Math.max(0, Math.floor(t))), u = t - i; return [C[i] + (C[i + 1] - C[i]) * u, S[i] + (S[i + 1] - S[i]) * u]; };
  const e0 = [650, -218, -1, 0, Math.PI], k = -1 / 120;
  const poseT = (g, w, tab) => {
    const gg = g - OB, a = e0[4] + k * gg, r = 1 / k + DNR;
    const [Cc, Ss] = tab ? cs(a) : [Math.cos(a), Math.sin(a)];
    const x = e0[0] + (Ss - e0[3]) * r, z = e0[1] - (Cc - e0[2]) * r;
    return [x - w * Ss, z + w * Cc];
  };
  let m = 0, px = 0;
  for (const f of frames) {
    const pc = f.g.pc;
    for (const p of f.poses) {
      if (where(p[1]) !== 'bend') continue;
      for (const w of p[2] === 0 ? [11, 22.6, 40] : [p[2]]) {
        const a = poseT(p[1], w, false), b = poseT(p[1], w, true), d = Math.hypot(a[0] - b[0], a[1] - b[1]);
        const Z = ((a[0] - pc[0]) * pc[2] + (a[1] - pc[1]) * pc[3]) / pc[4];
        m = Math.max(m, d);
        if (Z > .02) px = Math.max(px, d / Z);
      }
    }
  }
  return { m, px };
}

const sets = [['楕円 w=-100 コーナー 監督', true, -100, 0, x => x > OB && x < 650], ['楕円 w=-14 コーナー 監督', true, -14, 0, x => x > OB && x < 650],
  ['楕円 w=-100 コーナー WIDE 2', true, -100, 8, x => x > OB && x < 650], ['楕円 w=-14 コーナー WIDE 2', true, -14, 8, x => x > OB && x < 650],
  ['楕円 w=-100 ホームストレッチ 監督', true, -100, 0, x => x >= 650], ['楕円 w=-100 バックストレッチ 監督', true, -100, 0, x => x <= OB],
  ['直線 監督', false, -100, 0, () => true], ['直線 WIDE 2', false, -100, 8, () => true]];
const L = range(0, 1000, STEP), f1 = x => x.toFixed(1), f2 = x => (x / 1e3).toFixed(2);

console.log('### 段階 2: 1 フレームの pose と三角関数（host の回数は実測、ms は実機の単価からの推定）\n');
console.log('| 構成 | フレーム | pose | うち曲線上 | 曲線上の異なる g | sin+cos | pose の推定 ms | (b) g のキャッシュ | (c) 表+線形補間 | (d) VE の漸化式 | hoist | vetab | vetabS（採用） | fixtab |');
console.log('| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |');
const base = {};
for (const [name, oval, w2, cam, rf] of sets) {
  const R = {};
  for (const v of [null, 'hoist', 'vetab', 'vetabS', 'fixtab']) { const { out } = runSet(v, oval, w2, L, cam); R[v] = out.filter(f => rf(f.L)); }
  const fr = R[null], n = fr.length;
  const pc = poseCost(fr, oval);
  let bend = 0, dg = 0, trig = 0, ve = 0;
  for (const f of fr) {
    const b = f.poses.filter(p => oval && where(p[1]) === 'bend');
    bend += b.length; dg += new Set(b.map(p => p[1])).size; trig += (f.C['M.sin'] || 0) + (f.C['M.cos'] || 0);
    ve += f.poses.filter(p => p[0] === 'pan' && p[2] === 0 && where(p[1]) === 'bend').length;
  }
  bend /= n; dg /= n; trig /= n; ve /= n;
  // Savings, us a frame (positive = faster).
  const sB = (bend - dg) * (TRIG2 - MAP) - dg * 2 * MAP;
  const sC = bend * (TRIG2 - LERP);
  const sD = ve * (TRIG2 - REC);
  const tab = v => {
    const c = poseCost(R[v], oval);
    // Every pose that left: its cost; each VE entry and fp() read back.
    const veN = oval ? (R[null][0].g.CH.length) : 0, fpN = v === 'fixtab' ? (pc.n - c.n) - (oval ? veN : 0) - (v === 'fixtab' ? 0 : 0) : 0;
    let us = pc.us - c.us;
    if (v === 'vetab' || v === 'fixtab') us -= oval ? veN * VE_READ_EXTRA : 0;
    if (v === 'vetabS') us -= oval ? 17 * VE_SYM_EXTRA : 0;
    if (v === 'fixtab') {
      // fp() calls: the units (wide() 3 when the director runs it, shot() 1), the screen 2, the poles 6.
      const fpCalls = (cam ? 1 : 4) + 2 + 6;
      us -= fpCalls * FP;
    }
    return us;
  };
  console.log(`| ${name} | ${n} | ${f1(pc.n)} | ${f1(bend)} | ${f1(dg)} | ${f1(trig)} | ${f2(pc.us)} | ${f2(sB)} | ${f2(sC)} | ${f2(sD)} | ${f2(tab('hoist'))} | ${f2(tab('vetab'))} | ${f2(tab('vetabS'))} | ${f2(tab('fixtab'))} |`);
  base[name] = fr;
}
console.log(`\n単価（µs、推定を含む）: pose 曲線 ${POSE.bend}（実測）、楕円の直線区間 ${POSE.ovalStraight}（推定）、直線コースの近道 ${POSE.straight}（実測）、sin+cos ${TRIG2.toFixed(1)}（実測 18.7 × 2）、` +
  `(b) Map の get/set ${MAP.toFixed(1)}（推定）、(c) 補間 ${LERP.toFixed(1)}、(d) 回転 ${REC.toFixed(1)}、fp() ${FP.toFixed(1)}、VE の表の読み出しの差 ${VE_READ_EXTRA}（推定）`);

console.log('\n### (c) 表 + 線形補間の誤差（曲線上の全 pose、VE は w 11・22.6・40 で。コーナーの監督 w=-100 と w=-14 の最悪）\n');
console.log('| 分割数 N | 型 | 表の要素 | 表のバイト（データのみ） | 最大誤差 m | 最大誤差 px |');
console.log('| ---: | --- | ---: | ---: | ---: | ---: |');
for (const N of [64, 128, 256, 512]) for (const f32 of [true, false]) {
  let m = 0, px = 0;
  for (const k of ['楕円 w=-100 コーナー 監督', '楕円 w=-14 コーナー 監督']) { const e = lerpError(base[k], N, f32); m = Math.max(m, e.m); px = Math.max(px, e.px); }
  console.log(`| ${N} | ${f32 ? 'Float32Array' : '配列（double）'} | ${2 * (N + 1)} | ${2 * (N + 1) * (f32 ? 4 : 8)} | ${m.toExponential(2)} | ${px.toExponential(2)} |`);
}
