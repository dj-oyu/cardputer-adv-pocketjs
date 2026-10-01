// The chord bodies of DERBY WATCH's panning frames by their size on the panel
// (docs/apps/derby-trig-cull.md section 3): how many bodies, draws, VM steps
// and segments sit in bodies whose box is under 1..4 px, and the device time
// they cost (estimated from unit prices; trig_cull.mjs bodyCost()).
//
//   node tools/games/ovalcost/cull_stats.mjs [--step 5] [--variant name]
import { runSet, bodyBox, bodyCost, drawCost } from './trig_cull.mjs';

const arg = (k, d) => { const i = process.argv.indexOf(k); return i < 0 ? d : process.argv[i + 1]; };
const STEP = +arg('--step', 5), VARIANT = arg('--variant', null);
const OB = 650 - 120 * Math.PI;
const range = (a, b, s) => { const r = []; for (let x = a; x <= b + 1e-9; x += s) r.push(x); return r; };

export function bodyStats(frames) {
  const T = [1, 2, 3, 4], n = frames.length;
  const acc = { bodies: 0, draws: 0, steps: 0, seg: 0, us: 0, serPose: 0 };
  const small = T.map(() => ({ bodies: 0, draws: 0, steps: 0, seg: 0, us: 0 }));
  const bySeries = {};
  for (const f of frames) {
    acc.serPose += f.poses.filter(p => p[0].startsWith('ser:')).length;
    f.bodies.forEach((b, i) => {
      const box = bodyBox(b), d = f.draws.filter(x => x[2].startsWith('ser:') && x[3] === i);
      let st = 0, seg = 0;
      for (const [nm, a] of d) { const c = drawCost(nm, a); st += c[0]; seg += c[2]; }
      const us = bodyCost(b, d, 0);
      acc.bodies++; acc.draws += d.length; acc.steps += st; acc.seg += seg; acc.us += us;
      const key = b.n === 'prail' ? (b.b < 0 ? 'スタンド' : b.w === 11 ? '内柵' : '外柵') : b.n === 'pk' ? '観客' : '芝';
      const s = bySeries[key] || (bySeries[key] = { bodies: 0, lt2: 0 });
      s.bodies++; if (box.size < 2) s.lt2++;
      T.forEach((t, k) => { if (box.size < t) { const o = small[k]; o.bodies++; o.draws += d.length; o.steps += st; o.seg += seg; o.us += us; } });
    });
  }
  const per = o => Object.fromEntries(Object.entries(o).map(([k, v]) => [k, v / n]));
  return { frames: n, all: per(acc), small: small.map(per), bySeries: Object.fromEntries(Object.entries(bySeries).map(([k, v]) => [k, { bodies: v.bodies / n, lt2: v.lt2 / n }])) };
}

if (process.argv[1].endsWith('cull_stats.mjs')) {
  const bend = range(Math.ceil(OB), 648, STEP), straight = range(0, 1000, STEP);
  const f1 = x => x.toFixed(1), f0 = x => x.toFixed(0);
  console.log(`variant ${VARIANT || 'none'}; leader every ${STEP} m (bend ${f1(OB)}..650)`);
  console.log('| 構成 | フレーム | 本体/フレーム | draw | VM ステップ | 線分・点 | 本体の推定 ms | 箱 <1 px の本体 | <2 | <3 | <4 | <2 px の本体の推定 ms | <4 px の推定 ms |');
  console.log('| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |');
  const rows = [['楕円 w=-100 コーナー WIDE 2', true, -100, bend, 8], ['楕円 w=-14（u=25）コーナー WIDE 2', true, -14, bend, 8], ['楕円 w=-3（u=14）コーナー WIDE 2', true, -3, bend, 8],
    ['楕円 w=-100 コーナー 監督', true, -100, bend, 0], ['楕円 w=-14（u=25）コーナー 監督', true, -14, bend, 0], ['楕円 w=-3（u=14）コーナー 監督', true, -3, bend, 0],
    ['楕円 コーナー WIDE 1', true, -100, bend, 7], ['楕円 コーナー WIDE 3', true, -100, bend, 9],
    ['楕円 w=-14（u=25）ホームストレッチ 監督', true, -14, range(650, 1000, STEP), 0], ['楕円 w=-3（u=14）ホームストレッチ 監督', true, -3, range(650, 1000, STEP), 0],
    ['直線 WIDE 2', false, -100, straight, 8]];
  const all = {};
  for (const [name, oval, w2, L, cam] of rows) {
    const { out } = runSet(VARIANT, oval, w2, L, cam);
    const S = bodyStats(out); all[name] = S;
    console.log(`| ${name} | ${S.frames} | ${f1(S.all.bodies)} | ${f1(S.all.draws)} | ${f0(S.all.steps)} | ${f0(S.all.seg)} | ${(S.all.us / 1e3).toFixed(2)} | ` +
      S.small.map(o => `${f1(o.bodies)}`).join(' | ') + ` | ${(S.small[1].us / 1e3).toFixed(2)} | ${(S.small[3].us / 1e3).toFixed(2)} |`);
  }
  console.log('\n系列ごとの本体/フレーム（うち箱 <2 px）:');
  for (const [k, S] of Object.entries(all)) console.log(`  ${k}: ` + Object.entries(S.bySeries).map(([s, v]) => `${s} ${f1(v.bodies)} (${f1(v.lt2)})`).join(', '));
}
