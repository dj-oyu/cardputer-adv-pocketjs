// WIDE 2 on the oval at w -100 (now) and -3 (u 14): over the bend, every
// 10 m of the leader, which unit wide() picks, the leader's distance from it
// (e), f (at least CAMS[i][0] = 200, which it sits on below e = 34.3 m), the
// chords in view (VC) and where the field lands on the panel (x of the
// leader and the 8th runner at the rail's middle, panel 0..239).
// docs/apps/derby-trig-cull-device.md. Host, the app's own wide()/shot()/pan().
//
//   node tools/games/ovalcost/w3_check.mjs [--from 270] [--to 650] [--step 10]
import { runSet } from './trig_cull.mjs';

const arg = (k, d) => { const i = process.argv.indexOf(k); return i < 0 ? d : +process.argv[i + 1]; };
const FROM = arg('--from', 270), TO = arg('--to', 650), STEP = arg('--step', 10);
const L = [];
for (let x = FROM; x <= TO + 1e-9; x += STEP) L.push(x);
const NAMES = { 0: 'side', 7: 'WIDE 1', 8: 'WIDE 2', 9: 'WIDE 3' };
for (const w2 of [-100, -3]) {
  const { out, app } = runSet(null, true, w2, L, 0);
  const got = new Map(out.map(f => [f.L, f]));
  console.log(`\nWIDE 2 w=${w2} (u=${11 - w2})`);
  console.log('lead m | unit | e m | f | f on 200 | chord bodies | leader x | 8th x | e to W1 / W2 / W3');
  for (const x of L) {
    const f = got.get(x);
    const a = app.pose(x, (11 + 22.6) / 2), es = [7, 8, 9].map(i => {
      const c = f ? f.g.CAMS[i] : null, q = app.pose(c ? c[3] : [0, 0, 0, 0, 0, 0, 0, 100, 460, 820][i], c ? c[5] : -14);
      return Math.hypot(a[0] - q[0], a[1] - q[1]).toFixed(0);
    });
    if (!f) { console.log(`${x} | side | - | - | - | - | - | - | ${es.join(' / ')}`); continue; }
    const pc = f.g.pc, P = (g, w) => {
      // The panel x of the point (g, w), as pan() projects it (pc: x, z, aim, f).
      const m = app.pose(g, w), dx = m[0] - pc[0], dz = m[1] - pc[1];
      const Z = dx * pc[2] + dz * pc[3], X = dx * pc[3] - dz * pc[2];
      return Z > 0 ? (120 + X * pc[4] / Z).toFixed(0) : 'behind';
    };
    console.log(`${x} | ${NAMES[f.m]} | ${pc[5].toFixed(1)} | ${pc[4].toFixed(0)} | ${pc[4] <= 200.0001 ? 'yes' : ''} | ` +
      `${f.bodies.length} | ${P(x - 12.5 / 6, 12)} | ${P(x - 10.5 - 12.5 / 6, 12 * 1.085 ** 7)} | ${es.join(' / ')}`);
  }
}
