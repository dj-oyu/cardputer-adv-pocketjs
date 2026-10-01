// DERBY WATCH's crowd as shipped (P24: the pattern line,
// docs/kasane/crowd-primitives-design.md) beside the crowd before it (the
// checkerboard dots), in crowd_look.mjs's 12 scenes. Host only. Every
// picture is drawn by the firmware's own VM and band renderer
// (main/ui/kasane/ksn_procedural.c, through crowd_p24_host.c, compiled here
// with cc): the P24 column is apps/derby/derby_prog.js's crowd plan, compiled
// by tools/kasane_ir/plan_js.mjs, with the registration arguments and the
// inputs the app computes (derby_prog.js KN, derby_view.js course(),
// derby_pan.js ser()).
// Run it where cc is (WSL):
//
//   node tools/games/pancost/crowd_p24.mjs sheet [--out DIR]  derby-crowd-p24-preview.png, x3
//   node tools/games/pancost/crowd_p24.mjs gif [--out DIR]    derby-crowd-p24.gif, 8 frames
//   node tools/games/pancost/crowd_p24.mjs counts             entries, steps a scene
//
// The panning view's stretch is the union of the scene's crowd_look series
// (the straight is one chord: derby_pan.js draws it Lo..Hi, which the series
// cover to within a grid step, off the panel's edge). The look is not judged
// here; this only hands the pictures over.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import vm from 'node:vm';
import {execFileSync} from 'node:child_process';

process.env.NOMAIN = '1';
const L = await import('./crowd_look.mjs');
const {SCENES, sceneName, build, windowOf, rowsOf, grid, scale, png, gif, rgb, BG, ROOT} = L;
const {findPlans, compilePlan} = await import('../../kasane_ir/plan_js.mjs');
const {assemble} = await import('../../kasane_ir/kir.mjs');

// The app's own plan and look table: derby_prog.js's crowd and KN.
const PROG = fs.readFileSync(path.join(ROOT, 'apps/derby/derby_prog.js'), 'utf8');
const CROWD = compilePlan(findPlans(PROG, 'derby_prog.js').find(p => p.name === 'crowd'));
const KN = vm.runInNewContext(/^const KN = (\[[^;]*\]);$/m.exec(PROG)[1]);
const flo = Math.floor;

// P24's draws for a scene at frame t: [{rows, in}].
function p24(s, t) {
  const sc = build(s, t), k = KN[s.tier], rows = assemble(CROWD.code, [k[1], k[6], k[7]].concat(KN[3].slice(0, 5)));
  if (s.cam === 'side') {
    const {q, x0, gy} = sc.v, sx = w => 120 + (w - x0) * q;
    const j = flo((x0 - 130 / q) / 12), a = sx(j * 12), dx = 12 * q;
    const n = Math.min(flo((700 - a) / dx), Math.ceil((250 - a) / dx) + 1), y = gy - 1.2 * q;
    return [{rows, in: [a, y, a + n * dx, y, -2.4 * q, -2.4 * q, j * 12 * KN[3][5], (j + n) * 12 * KN[3][5]]}];
  }
  const mark = {}, list = sc.cand({pan: mark, per: 1}).filter(d => d.plan === mark);
  let lo = null, hi = null;
  for (const d of list) {
    const [xn, dxn, pz, dz, n] = d.in;
    for (const i of [0, n]) {
      const z = -(pz + i * dz), p = {g: d.wx + i * d.wk, x: (xn + i * dxn) / z, z};
      if (!lo || p.g < lo.g) lo = p;
      if (!hi || p.g > hi.g) hi = p;
    }
  }
  if (!lo || lo.g === hi.g) return [];
  return [{rows, in: [lo.x, 28 + 4.8 / lo.z, hi.x, 28 + 4.8 / hi.z, -2.4 / lo.z, -2.4 / hi.z, lo.g * KN[3][5], hi.g * KN[3][5]]}];
}
// A picture's draws: the scenery, then today's crowd or P24's.
function draws(s, t, which) {
  const sc = build(s, t), old = d => ({rows: rowsOf(d.plan, d.args), in: d.in});
  if (which === 'today') return sc.today.map(old);
  return sc.today.filter(d => d.bg).map(old).concat(p24(s, t));
}

const BACK = (BG[0] >> 3) << 11 | (BG[1] >> 2) << 5 | BG[2] >> 3;
let tool;
function host(pictures) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'crowd-p24-'));
  if (!tool) {
    tool = path.join(dir, 'crowd_p24_host');
    const K = path.join(ROOT, 'main/ui/kasane');
    execFileSync('cc', ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-I' + K,
      path.join(K, 'ksn_procedural.c'), path.join(K, 'ksn_proc_analysis.c'), path.join(K, 'ksn_proc_plan.c'),
      path.join(ROOT, 'tools/games/pancost/crowd_p24_host.c'), '-lm', '-o', tool], {stdio: 'inherit'});
  }
  const text = [];
  for (const p of pictures) {
    text.push(`FRAME ${BACK}`);
    for (const d of p) text.push(`D ${d.rows.length}`, ...d.rows.map(r => r.join(' ')), 'I ' + d.in.join(' '));
    text.push('ENDFRAME');
  }
  const raw = path.join(dir, 'out.rgb565');
  const lines = execFileSync(tool, [raw], {input: text.join('\n') + '\n', maxBuffer: 1 << 28}).toString().trim().split('\n');
  const buf = fs.readFileSync(raw);
  return pictures.map((_, i) => ({px: new Uint16Array(buf.buffer, buf.byteOffset + i * 240 * 135 * 2, 240 * 135),
    counts: lines[i].split(' ').map(Number)}));
}
function crop(px, win) {
  const h = win[1] - win[0], d = new Uint8Array(240 * h * 3);
  for (let y = 0; y < h; y++) for (let x = 0; x < 240; x++) {
    const c = px[(y + win[0]) * 240 + x];
    d.set(c === BACK ? BG : rgb(c), (y * 240 + x) * 3);
  }
  return {w: 240, h, d};
}
const argv = process.argv.slice(2), outDir = argv.includes('--out') ? path.resolve(argv[argv.indexOf('--out') + 1]) : path.join(ROOT, 'docs/apps');
const COLS = ['today', 'p24'];
function sheet() {
  const res = host(SCENES.flatMap(s => COLS.map(c => draws(s, 0, c))));
  const cells = SCENES.map((s, i) => { const win = windowOf(s); return COLS.map((_, j) => crop(res[i * COLS.length + j].px, win)); });
  png(path.join(outDir, 'derby-crowd-p24-preview.png'), scale(grid(cells), +(process.env.SCALE || 3)));
  console.log('rows: ' + SCENES.map(sceneName).join(', '));
  console.log('columns: today (checkerboard), P24 (as shipped); t = 0, x3');
}
function gifs() {
  const wins = SCENES.map(windowOf), jobs = [], frames = [];
  for (let t = 0; t < 8; t++) for (const s of SCENES) jobs.push(draws(s, t, 'today'), draws(s, t, 'p24'));
  const res = host(jobs);
  for (let t = 0; t < 8; t++)
    frames.push(scale(grid(SCENES.map((_, i) => [0, 1].map(j => crop(res[(t * SCENES.length + i) * 2 + j].px, wins[i])))), 2));
  gif(path.join(outDir, 'derby-crowd-p24.gif'), frames, 3);
  console.log('columns today | P24, rows as the sheet, t = 0..7 at race pace (16 m/s), x2, 30 ms a frame');
}
// The crowd draws' own counts (the P24 column minus the scenery is the
// crowd): steps, frame entries, raster steps, a mean over 8 frames.
function counts() {
  console.log('scene | crowd draws | steps | entries | raster steps (P24, mean of 8 frames)');
  for (const s of SCENES) {
    const res = host([...Array(8)].map((_, t) => p24(s, t))), m = j => res.reduce((a, r) => a + r.counts[j], 0) / 8;
    console.log(`${sceneName(s)} | ${m(0).toFixed(1)} | ${m(1).toFixed(0)} | ${m(2).toFixed(0)} | ${m(3).toFixed(0)}`);
  }
}
switch (argv[0]) {
  case 'sheet': sheet(); break;
  case 'gif': gifs(); break;
  case 'counts': counts(); break;
  default: console.log('usage: crowd_p24.mjs sheet|gif|counts [--out DIR]'); process.exit(2);
}
