// Stage 3 of docs/apps/derby-trig-cull.md: what each culling variant
// (variants.mjs) removes from a panning frame of DERBY WATCH's oval, counted
// on the host (exact for the scripted frames), and the device time that saves
// (estimates, two ways: the hand-counted body model of trig_cull.mjs, and the
// device's average ser() body of 1.2-1.5 ms, derby-pan-memory.md section 3).
// Also the candidates of (c): bodies whose panel span lies inside a nearer
// body's of the same series.
//
//   node tools/games/ovalcost/cull_cost.mjs [--step 5]
import { runSet, bodyBox, P } from './trig_cull.mjs';
import { bodyStats } from './cull_stats.mjs';

const arg = (k, d) => { const i = process.argv.indexOf(k); return i < 0 ? d : process.argv[i + 1]; };
const STEP = +arg('--step', 5);
const OB = 650 - 120 * Math.PI;
const range = (a, b, s) => { const r = []; for (let x = a; x <= b + 1e-9; x += s) r.push(x); return r; };
// dv(): one call, both depths, one end between (hand count of variants.mjs
// MERGEF: reads, mul, add, div, sqrt, abs, max, compares): about 410 us (estimate).
const DV = 410;
// dev() (merge, per series): three ends through closures and 2-arrays, both
// heights: about 480 us a call (estimate).
const DEV = 480;

function count(out) {
  const S = bodyStats(out), n = out.length;
  let serPose = 0, serPoseBend = 0, dv = 0, calls = 0, hidden = 0;
  for (const f of out) {
    for (const p of f.poses) if (p[0].startsWith('ser:')) { ++serPose; if (p[1] > OB && p[1] <= 650) ++serPoseBend; }
    dv += (f.C.dv || 0) * DV + (f.C.dev || 0) * DEV; calls += (f.C.dv || 0) + (f.C.dev || 0);
    const B = f.bodies.map(b => ({ b, x: bodyBox(b) }));
    for (const A of B) if (B.some(C => C !== A && C.b.n === A.b.n && C.b.w === A.b.w && C.x.zFar < A.x.zNear && C.x.x0 <= A.x.x0 && C.x.x1 >= A.x.x1)) ++hidden;
  }
  return { ...S.all, serPose: serPose / n, serPoseBend: serPoseBend / n, dv: dv / n, calls: calls / n, hidden: hidden / n };
}

const sets = [['楕円 w=-100 コーナー WIDE 2', -100, 8, range(Math.ceil(OB), 648, STEP)], ['楕円 w=-14（u=25）コーナー WIDE 2', -14, 8, range(Math.ceil(OB), 648, STEP)], ['楕円 w=-3（u=14）コーナー WIDE 2', -3, 8, range(Math.ceil(OB), 648, STEP)],
  ['楕円 コーナー WIDE 1', -100, 7, range(Math.ceil(OB), 648, STEP)], ['楕円 コーナー WIDE 3', -100, 9, range(Math.ceil(OB), 648, STEP)],
  ['楕円 w=-14（u=25）ホームストレッチ 監督', -14, 0, range(650, 1000, STEP)], ['楕円 w=-3（u=14）ホームストレッチ 監督', -3, 0, range(650, 1000, STEP)]];
const V = ['cull1', 'cull2', 'cull3', 'cull4', 'merge1', 'merge2', 'mergeF0.5', 'mergeF1', 'mergeF2'];
const f1 = x => x.toFixed(1), f2 = x => x.toFixed(2);
console.log('| 構成 | 案 | 本体 | draw | VM ステップ | 線分・点 | ser の pose | 判定の呼び出し | 推定の削減 ms（モデル） | 推定の削減 ms（実機の本体 1.2〜1.5 ms） |');
console.log('| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |');
for (const [name, w2, cam, L] of sets) {
  const b = count(runSet(null, true, w2, L, cam).out);
  console.log(`| ${name} | 今 | ${f1(b.bodies)} | ${f1(b.draws)} | ${b.steps.toFixed(0)} | ${b.seg.toFixed(0)} | ${f1(b.serPose)} | 0 | — | — |`);
  for (const v of V) {
    const c = count(runSet(v, true, w2, L, cam).out);
    const model = (b.us - c.us) / 1e3 + (b.serPoseBend - c.serPoseBend) * P.poseBend / 1e3 + ((b.serPose - b.serPoseBend) - (c.serPose - c.serPoseBend)) * 70 / 1e3 - c.dv / 1e3;
    const dev = [P.serBodyLo, P.serBodyHi].map(u => ((b.bodies - c.bodies) * u - c.dv) / 1e3);
    console.log(`| | ${v} | ${f1(c.bodies)} | ${f1(c.draws)} | ${c.steps.toFixed(0)} | ${c.seg.toFixed(0)} | ${f1(c.serPose)} | ${f1(c.calls)} | ${f2(model)} | ${f2(dev[0])}〜${f2(dev[1])} |`);
  }
  console.log(`| | (c) 手前の同じ系列に横幅が収まる本体の数 | ${f1(b.hidden)} | | | | | | | |`);
}
