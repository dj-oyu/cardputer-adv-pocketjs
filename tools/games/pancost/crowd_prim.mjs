// Crowd primitives for DERBY WATCH (docs/kasane/crowd-primitives-design.md):
// the pattern line (KSN_PROC_LINE_PATTERN) and the tile rectangle
// (KSN_PROC_TILE), prototypes in main/ui/kasane/ksn_procedural.c. Host only.
// The scenes are crowd_look.mjs's (LIGHT/MID/HEAVY x {side WIDE, panning
// WIDE} x leader at 300 / 800 m, the panning unit forced). Every picture is
// drawn by the firmware's own VM and band renderer: this writes the draws
// (hand IR rows and inputs) for tools/games/pancost/crowd_prim_host.c, which
// it compiles with cc. Run it where cc is (WSL):
//
//   node tools/games/pancost/crowd_prim.mjs table            counts and estimated cost
//   node tools/games/pancost/crowd_prim.mjs sheet [--out DIR]  one PNG, x3
//   node tools/games/pancost/crowd_prim.mjs gif [--out DIR]    8 frames a candidate
//   node tools/games/pancost/crowd_prim.mjs tiles [--out DIR]  the tiles' frames, x6
//   node tools/games/pancost/crowd_prim.mjs check            today's pictures = crowd_look's
//
// The look is not judged here; this only hands the pictures and counts over.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {execFileSync} from 'node:child_process';

const quiet = process.env.NOMAIN;
process.env.NOMAIN = '1';
const L = await import('./crowd_look.mjs');
const {SCENES, KN, sceneName, build, windowOf, rowsOf, grid, scale, png, gif, rgb, BG, ROOT} = L;
const PI = Math.PI, flo = Math.floor;

// ------------------------------------------------------------ hand IR
// Rows [op, dst, a, b, value, color] (ksn_proc_inst). 15 and 16 are the
// prototype ops.
const S = (r, v) => [0, r, 0, 0, v, 0], I = (r, n) => [1, r, n, 0, 0, 0];
const A = (d, a, b) => [2, d, a, b, 0, 0], M = (d, a, b) => [3, d, a, b, 0, 0];
const REP = n => [5, 0, n, 0, 0, 0], REPR = r => [10, 0, r, 0, 0, 0], END = [6, 0, 0, 0, 0, 0];
const MOVE = (a, b) => [7, 0, a, b, 0, 0];
const PAT = (base, a, b, n) => [15, base, a, b, n, 0];
const TILE = (base, a, b, id) => [16, base, a, b, 0, id];

const RED = 0xc228, BLUE = 0x3a7a, YEL = 0xe6c4, GRN = 0x5d2b, VIO = 0xa2b5, TAN = 0x8c51, PALE = 0xdefb;
const SKIN = 0xf5d3, DARK = 0xb3a9, HAIR = 0x4208, KEY = 0xf81f;

// "fan": rows of pattern lines between two ends, a row a (dy0, dy1) apart
// at each end. The side view is the flat case (y0 = y1, dy0 = dy1); a
// stretch of the panning view's stand is the other (its rows are straight
// lines that fan out towards the near end). Inputs: x0, y0, x1, y1, dy0,
// dy1, u0, u1 (pattern cells at the two ends). A row is three lines: the
// head 2 px above the row's line, the body on it and 1 px above.
// The rows' steps (dy0, dy1 = -2.4 m / Z') are also the two ends' depth
// weights: the VM cuts a line whose ends differ in depth into chords, with
// the pattern laid in perspective (none in the side view).
//   r0..r3 the ends, r4..r10 the block (pattern, A, B, u0, u1, dy0, dy1),
//   r11 = -1, r12, r13 the offset ends, r14 the colour flip's sum, r15 the
//   row's cloth.
// noise: N = 0, the pattern is density << 16 | seed, one less a row. Else
// N = 24 with a head and a body pattern, moved 7 cells a row.
function fanRows(rows, o) {
  const head = o.noise ? [S(5, SKIN), S(6, HAIR)] : [S(4, o.head), S(5, SKIN)];
  const body = o.noise ? [M(5, 15, 11), M(5, 5, 11), S(6, PALE)] : [S(4, o.body), M(5, 15, 11), M(5, 5, 11)];
  const next = o.noise ? [A(4, 4, 11)] : [S(12, 7), A(7, 7, 12), A(8, 8, 12)];
  return [
    I(0, 0), I(1, 1), I(2, 2), I(3, 3), I(9, 4), I(10, 5), I(7, 6), I(8, 7),
    S(11, -1), S(14, RED + BLUE), S(15, RED), S(4, o.noise ? o.density << 16 | 4000 : o.head), S(6, -1),
    REP(rows),
    ...head,
    A(12, 1, 11), A(12, 12, 11), A(13, 3, 11), A(13, 13, 11), MOVE(0, 12), PAT(4, 2, 13, o.noise ? 0 : 24),
    ...body,
    MOVE(0, 1), PAT(4, 2, 3, o.noise ? 0 : 24),
    A(12, 1, 11), A(13, 3, 11), MOVE(0, 12), PAT(4, 2, 13, o.noise ? 0 : 24),
    M(15, 15, 11), A(15, 15, 14),
    ...next,
    A(1, 1, 9), A(3, 3, 10),
    END,
  ];
}
// The 24-cell patterns: people 3 cells wide (a cell is .4 m), the head on
// the middle one. Bit c is cell c.
const bits = (starts, w, off) => starts.reduce((b, s) => { for (let i = 0; i < w; i++) b |= 1 << (s + off + i); return b; }, 0) >>> 0;
const SEATS = [[0, 9, 17], [0, 5, 10, 14, 19], [0, 4, 8, 12, 16, 20]];
const NOISE_DENSITY = [90, 140, 190], NOISE_CELL = .8, P24_CELL = .4;

// Tiles: 64 x 24 texels (.4 m a texel: 1:1 in the side WIDE view), four rows
// of seats 6 texels high, 16 seats a row; a seat is taken by a hash under
// the tier's density. A person: head, shoulders 3 wide, a darker torso.
// Frame 1 lifts a quarter of them by a texel, frame 2 raises another
// quarter's arms.
const TW = 64, TH = 24, TF = 3, TEXEL = .4;
const hash = n => { n = Math.imul(n ^ n >>> 16, 0x45d9f3b); n = Math.imul(n ^ n >>> 16, 0x45d9f3b); return (n ^ n >>> 16) >>> 0; };
const CLOTH = [RED, BLUE, YEL, GRN, VIO, PALE, TAN];
function makeTile(density, seed) {
  const px = new Uint16Array(TW * TH * TF).fill(KEY);
  const put = (f, x, y, c) => { if (y >= 0 && y < TH) px[(f * TH + y) * TW + x] = c; };
  for (let row = 0; row < 4; row++) for (let seat = 0; seat < 16; seat++) {
    const h = hash(seed * 64 + row * 16 + seat + 1);
    if ((h & 255) >= density) continue;
    const x = 4 * seat + (h >> 8 & 1), cloth = CLOTH[(h >>> 9) % CLOTH.length], top = [SKIN, SKIN, DARK, HAIR][h >>> 13 & 3];
    const shade = cloth >> 1 & 0x7bef, jumps = (h >>> 16 & 3) === 0, cheers = (h >>> 16 & 3) === 1;
    for (let f = 0; f < TF; f++) {
      const y = row * 6 + 1 - (f === 1 && jumps ? 1 : 0);
      put(f, x + 1, y, top);
      for (let i = 0; i < 3; i++) { put(f, x + i, y + 1, cloth); put(f, x + i, y + 2, shade); }
      if (f === 2 && cheers) { put(f, x, y, cloth); put(f, x + 2, y, cloth); }
    }
  }
  return {w: TW, h: TH, frames: TF, key: KEY, px};
}
const TILES = [makeTile(100, 1), makeTile(165, 2), makeTile(225, 3)];
// The side view: one rectangle, the stand's rows. Inputs: u0, u1 (texels at
// the two edges), top, bottom, frame.
const tileSide = (rows, id) => [
  S(0, 0), I(1, 2), S(2, 239), I(3, 3),
  I(4, 0), S(5, TH - 6 * rows), I(6, 1), S(7, TH), I(8, 4),
  MOVE(0, 1), TILE(4, 2, 3, id),
];
// The panning view: a rectangle a cell of crowd_look's ser() series, its
// size from 1/Z' by three Newton steps a cell (as pk). Inputs are pk's with
// lengths in texels (x, dx, z, dz over .4; r times .4), then u + 4096 *
// frame at the series' start and the texels a cell. All 16 registers:
//   r0 px, r1 pz, r2 q, r3 dx, r4 dz, r5 texels a cell, r6..r10 the block
//   (u0, v0, u1, v1, frame), r11 28 (the horizon), r12 the top and r13 the
//   bottom in texels under the horizon, r14 a temporary, r15 the cell's x.
const tilePan = (rows, id) => [
  I(0, 0), I(3, 1), I(1, 2), I(4, 3), I(14, 4), I(2, 5), I(6, 6), I(5, 7),
  S(7, TH - 6 * rows), S(9, TH),
  S(11, 1 / 4096), M(10, 6, 11), S(11, 28), S(12, 15 - 6 * rows), S(13, 15),
  M(15, 0, 2),
  REPR(14),
  M(14, 2, 12), A(14, 14, 11), MOVE(15, 14),
  A(0, 0, 3), A(1, 1, 4),
  M(14, 1, 2), M(14, 14, 2), A(14, 14, 2), A(2, 14, 2),
  M(14, 1, 2), M(14, 14, 2), A(14, 14, 2), A(2, 14, 2),
  M(14, 1, 2), M(14, 14, 2), A(14, 14, 2), A(2, 14, 2),
  M(15, 0, 2),
  A(8, 6, 5),
  M(14, 2, 13), A(14, 14, 11),
  TILE(6, 15, 14, id),
  A(6, 6, 5),
  END,
];

const CANDS = [
  {id: 'P24', label: '模様線（24 ビット）', kind: 'fan'},
  {id: 'PN', label: 'ノイズ線', kind: 'fan', noise: true},
  {id: 'T', label: 'タイル矩形', kind: 'tile'},
];
const mod = (v, m) => v - Math.floor(v / m) * m;
// A candidate's crowd draws for a scene at frame t.
function crowd(c, s, t) {
  const sc = build(s, t), rows = KN[s.tier][1], out = [];
  const fan = c.kind === 'fan' && fanRows(rows, c.noise ? {noise: true, density: NOISE_DENSITY[s.tier]}
    : {head: bits(SEATS[s.tier], 1, 1), body: bits(SEATS[s.tier], 3, 0)});
  const cell = c.noise ? NOISE_CELL : P24_CELL, period = c.noise ? 256 : 24;
  const shot = (t >> 1) % TF;
  if (s.cam === 'side') {
    const {q, x0, gy} = sc.v, w0 = x0 - 120 / q; // the stand at screen x = 0, m
    if (fan) {
      const u = mod(w0 / cell, period);
      out.push({rows: fan, in: [0, gy - 1.2 * q, 239, gy - 1.2 * q, -2.4 * q, -2.4 * q, u, u + 239 / q / cell]});
    } else {
      const u = mod(w0 / TEXEL, TW);
      out.push({rows: tileSide(rows, s.tier), in: [u, u + 240 / q / TEXEL, gy - 2.4 * q * rows, gy - 1, shot, 0, 0, 0]});
    }
    return out;
  }
  // The panning view: crowd_look's series along the stand (z = 40 m), 12 m
  // apart for the fan (only a stretch's two ends are used), 6 m for tiles.
  const mark = {}, list = sc.cand({pan: mark, per: fan ? 1 : 2}).filter(d => d.plan === mark);
  for (const d of list) {
    const [xn, dxn, pz, dz, n] = d.in;
    if (fan) {
      const za = -pz, zb = -(pz + n * dz), u = mod(d.wx / cell, period);
      out.push({rows: fan, in: [xn / za, 28 + 4.8 / za, (xn + n * dxn) / zb, 28 + 4.8 / zb, -2.4 / za, -2.4 / zb, u, u + n * d.wk / cell]});
    } else
      out.push({rows: tilePan(rows, s.tier), in: [xn / TEXEL, dxn / TEXEL, pz / TEXEL, dz / TEXEL, n, d.in[5] * TEXEL,
        mod(d.wx / TEXEL, TW) + 4096 * shot, d.wk / TEXEL]});
  }
  return out;
}
// A picture's draws: [rows, inputs, kind]; kind 0 scenery, 1 crowd, 2 fill.
function draws(s, t, which) {
  const sc = build(s, t), old = d => ({rows: rowsOf(d.plan, d.args), in: d.in, kind: d.bg ? 0 : d.fill ? 2 : 1});
  if (which === 'today') return sc.today.map(old);
  return sc.today.filter(d => d.bg).map(old).concat(crowd(which, s, t).map(d => ({...d, kind: 1})));
}

// ------------------------------------------------------------ the C side
const BACK = (BG[0] >> 3) << 11 | (BG[1] >> 2) << 5 | BG[2] >> 3;
const NAMES = ['draws', 'steps', 'sins', 'entries', 'ext', 'raster', 'scans', 'hits', 'linePx', 'patPx', 'patWritten', 'patWalkPx', 'tileRows', 'tilePx', 'tileWritten'];
let tool;
function host(pictures) { // pictures: lists of draws -> [{I, crowd, fill}]
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'crowd-prim-'));
  if (!tool) {
    tool = path.join(dir, 'crowd_prim_host');
    const K = path.join(ROOT, 'main/ui/kasane');
    execFileSync('cc', ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-DKSN_PROC_STATS', '-I' + K,
      path.join(K, 'ksn_procedural.c'), path.join(K, 'ksn_proc_analysis.c'), path.join(K, 'ksn_proc_plan.c'),
      path.join(ROOT, 'tools/games/pancost/crowd_prim_host.c'), '-lm', '-o', tool], {stdio: 'inherit'});
  }
  const text = [`TILES ${TILES.length}`];
  for (const t of TILES) text.push(`${t.w} ${t.h} ${t.frames} ${t.key}`, Array.from(t.px).join(' '));
  for (const p of pictures) {
    text.push(`FRAME ${BACK}`);
    for (const d of p) {
      if (d.in.length !== 8) throw new Error('8 inputs');
      text.push(`D ${d.rows.length} ${d.kind}`, ...d.rows.map(r => r.join(' ')), 'I ' + d.in.join(' '));
    }
    text.push('ENDFRAME');
  }
  const raw = path.join(dir, 'out.rgb565');
  const lines = execFileSync(tool, [raw], {input: text.join('\n') + '\n', maxBuffer: 1 << 28}).toString().trim().split('\n');
  const buf = fs.readFileSync(raw);
  return pictures.map((_, i) => {
    const v = lines[i].split(' ').map(Number), named = at => Object.fromEntries(NAMES.map((k, j) => [k, v[at + j]]));
    return {px: new Uint16Array(buf.buffer, buf.byteOffset + i * 240 * 135 * 2, 240 * 135), crowd: named(0), fill: named(NAMES.length)};
  });
}
function crop(px, win) {
  const h = win[1] - win[0], d = new Uint8Array(240 * h * 3);
  for (let y = 0; y < h; y++) for (let x = 0; x < 240; x++) {
    const c = px[(y + win[0]) * 240 + x];
    d.set(c === BACK ? BG : rgb(c), (y * 240 + x) * 3);
  }
  return {w: 240, h, d};
}

// Estimate (not measured). The draw side is crowd_look's fit to today's
// crowd on the device (50 us a draw, .45 a step, 1.0 a SIN, 1.0 an entry
// emitted) with an extended entry's emission put at 3 us (pattern line) and
// 5 us (tile): float divides, fmodf, lroundf (docs/perf/pie-simd.md 3.2).
// The band side is docs/kasane/derby-background-cost.md's measured model
// for lines (50 ns an entry x band test, .35 us an entry reaching a band
// with its first pixel, .143 us a further pixel), and for the new pixels
// the instructions of the -O2 Xtensa build's loops at the line loop's
// measured 1.1 cycles an instruction: a pattern pixel on the horizontal
// path .0625 us (noise .11), on the walk .2 (noise .27); a tile pixel .05
// and .25 a tile row; .6 an extended entry reaching a band.
function estimate(c, noise) {
  const plain = c.entries - 2 * c.ext, pattern = c.patPx > 0 || (c.ext && !c.tilePx && !c.tileRows);
  const draw = 50 * c.draws + .45 * c.steps + 1.0 * c.sins + 1.0 * plain + (pattern ? 3 : 5) * c.ext;
  const extHits = c.ext ? c.hits : 0, plainHits = c.ext ? 0 : c.hits; // a crowd draw is one kind or the other
  const flat = c.patPx - c.patWalkPx;
  const band = .05 * c.scans + .35 * plainHits + .143 * Math.max(0, c.linePx - plainHits) + .6 * extHits
    + (noise ? .11 : .0625) * flat + (noise ? .27 : .2) * c.patWalkPx + .25 * c.tileRows + .05 * c.tilePx;
  return {draw, band, total: draw + band};
}

const argv = process.argv.slice(2), outDir = argv.includes('--out') ? path.resolve(argv[argv.indexOf('--out') + 1]) : path.join(ROOT, 'docs/apps');
const COLS = ['today', ...CANDS], idOf = c => c === 'today' ? 'today' : c.id;
function table() {
  const jobs = [];
  for (const s of SCENES) for (const c of COLS) for (let t = 0; t < 8; t++) jobs.push(draws(s, t, c));
  const res = host(jobs);
  console.log('instructions: fan ' + fanRows(3, {noise: true, density: 1}).length + ' (noise) / ' + fanRows(3, {head: 1, body: 1}).length
    + ' (24 bit), tile side ' + tileSide(3, 0).length + ', tile pan ' + tilePan(3, 0).length);
  console.log('tiles: ' + TILES.length + ' x ' + TW + 'x' + TH + ' x ' + TF + ' frames, RGB565: ' + TILES.length * TW * TH * TF * 2 + ' B of const data');
  console.log('\nscene | form | draws | steps | SIN | entries | of which extended | band tests | band hits | line px | pattern px (written) | on the walk | tile rows | tile px (written) | draw us (est) | band us (est) | total us (est) | x today');
  const sum = {};
  let k = 0;
  for (const s of SCENES) {
    let base = 0;
    for (const c of COLS) {
      const acc = Object.fromEntries(NAMES.map(n => [n, 0]));
      for (let t = 0; t < 8; t++, k++) for (const n of NAMES) acc[n] += res[k].crowd[n] / 8;
      const e = estimate(acc, c.noise), f = v => v.toFixed(0);
      if (c === 'today') base = e.total;
      (sum[idOf(c)] ??= []).push([e.draw, e.band, e.total, e.total / base]);
      console.log(`${sceneName(s)} | ${idOf(c)} | ${acc.draws.toFixed(1)} | ${f(acc.steps)} | ${f(acc.sins)} | ${f(acc.entries)} | ${f(acc.ext)} | ${f(acc.scans)} | ${f(acc.hits)} | ${f(acc.linePx)} | ${f(acc.patPx)} (${f(acc.patWritten)}) | ${f(acc.patWalkPx)} | ${f(acc.tileRows)} | ${f(acc.tilePx)} (${f(acc.tileWritten)}) | ${f(e.draw)} | ${f(e.band)} | ${f(e.total)} | ${(e.total / base).toFixed(2)}`);
    }
  }
  console.log('\nmeans over the 12 scenes (estimates): form | draw us | band us | total us | mean of the ratios to today | side scenes only | panning only');
  for (const [id, v] of Object.entries(sum)) {
    const m = (j, pick = () => true) => { const u = v.filter((_, i) => pick(SCENES[i])); return u.reduce((a, b) => a + b[j], 0) / u.length; };
    console.log(`${id} | ${m(0).toFixed(0)} | ${m(1).toFixed(0)} | ${m(2).toFixed(0)} | ${m(3).toFixed(2)} | ${m(3, s => s.cam === 'side').toFixed(2)} | ${m(3, s => s.cam === 'pan').toFixed(2)}`);
  }
}
function sheet() {
  const res = host(SCENES.flatMap(s => COLS.map(c => draws(s, 0, c))));
  const cells = SCENES.map((s, i) => { const win = windowOf(s); return COLS.map((_, j) => crop(res[i * COLS.length + j].px, win)); });
  png(path.join(outDir, 'derby-crowd-prim-preview.png'), scale(grid(cells), +(process.env.SCALE || 3)));
  console.log('rows: ' + SCENES.map(sceneName).join(', '));
  console.log('columns: today, ' + CANDS.map(c => c.id + ' ' + c.label).join(', ') + '; t = 0, x3');
}
function gifs() {
  const wins = SCENES.map(windowOf);
  for (const c of CANDS) {
    const jobs = [];
    for (let t = 0; t < 8; t++) for (const s of SCENES) jobs.push(draws(s, t, 'today'), draws(s, t, c));
    const res = host(jobs), frames = [];
    for (let t = 0; t < 8; t++)
      frames.push(scale(grid(SCENES.map((_, i) => [0, 1].map(j => crop(res[(t * SCENES.length + i) * 2 + j].px, wins[i])))), 2));
    gif(path.join(outDir, `derby-crowd-prim-${c.id}.gif`), frames, 3);
  }
  console.log('each GIF: columns today | candidate, rows as the sheet, t = 0..7 at race pace (16 m/s), x2, 30 ms a frame');
}
function tiles() {
  const cells = TILES.map(t => [...Array(TF)].map((_, f) => {
    const d = new Uint8Array(TW * TH * 3);
    for (let i = 0; i < TW * TH; i++) { const c = t.px[f * TW * TH + i]; d.set(c === KEY ? BG : rgb(c), i * 3); }
    return {w: TW, h: TH, d};
  }));
  // grid() lays 240-wide cells; the tiles are 64 wide, so paste by hand.
  const gap = 2, W = TF * TW + (TF - 1) * gap, H = TILES.length * TH + (TILES.length - 1) * gap, d = new Uint8Array(W * H * 3).fill(255);
  cells.forEach((r, i) => r.forEach((c, j) => { for (let y = 0; y < TH; y++) d.set(c.d.subarray(y * TW * 3, (y + 1) * TW * 3), ((i * (TH + gap) + y) * W + j * (TW + gap)) * 3); }));
  png(path.join(outDir, 'derby-crowd-prim-tiles.png'), scale({w: W, h: H, d}, 6));
  console.log('rows: LIGHT, MID, HEAVY; columns: frames 0, 1, 2; x6');
}
// The pipeline against crowd_look's: today's pictures, pixel for pixel. The
// segments are crowd_look's stepper's; the raster here is ksn_proc_render_band's
// walk (crowd_look's own breaks Bresenham ties the other way, a pixel's
// difference on some slanted lines, counted below).
function walk(segs, y0, h) {
  const d = new Uint8Array(240 * h * 3);
  for (let i = 0; i < 240 * h; i++) d.set(BG, i * 3);
  for (const [xa, ya, xb, yb, c] of segs) {
    const col = c === BACK ? BG : rgb(c), dx = Math.abs(xb - xa), dy = Math.abs(yb - ya), sx = xa < xb ? 1 : -1, sy = ya < yb ? 1 : -1;
    let x = xa, y = ya, err = dx - dy;
    for (;;) {
      if (x >= 0 && x < 240 && y - y0 >= 0 && y - y0 < h) d.set(col, ((y - y0) * 240 + x) * 3);
      if (x === xb && y === yb) break;
      const twice = 2 * err;
      if (twice > -dy) { err -= dy; x += sx; }
      if (twice < dx) { err += dx; y += sy; }
    }
  }
  return d;
}
function check() {
  const jobs = [], wins = SCENES.map(windowOf);
  for (const s of SCENES) for (let t = 0; t < 8; t++) jobs.push(draws(s, t, 'today'));
  const res = host(jobs);
  let k = 0, bad = 0, ties = 0;
  for (const [i, s] of SCENES.entries()) for (let t = 0; t < 8; t++, k++) {
    const segs = L.run(build(s, t).today).segs, h = wins[i][1] - wins[i][0];
    const a = Buffer.from(crop(res[k].px, wins[i]).d);
    if (Buffer.compare(a, Buffer.from(walk(segs, wins[i][0], h)))) { bad++; console.log('differs: ' + sceneName(s) + ' t=' + t); }
    if (Buffer.compare(a, Buffer.from(L.raster(segs, wins[i][0], h).d))) ties++;
  }
  console.log(bad ? 'CHECK FAIL' : `CHECK PASS (${k} pictures of today's crowd: the C VM and band renderer = crowd_look.mjs's stepper drawn with the renderer's walk; ${ties} of them differ from crowd_look's own raster, which breaks ties the other way)`);
  process.exitCode = bad ? 1 : 0;
}
export {CANDS, TILES, crowd, draws};
if (!quiet) switch (argv[0]) {
  case 'table': table(); break;
  case 'sheet': sheet(); break;
  case 'gif': gifs(); break;
  case 'tiles': tiles(); break;
  case 'check': check(); break;
  case 'dump': { // dump SCENE CAND X0 Y0 W H: the picture's RGB565 values (debugging)
    const [, si, ci, x0, y0, w, h] = argv.map(Number), r = host([draws(SCENES[si], 0, COLS[ci])])[0];
    for (let y = y0; y < y0 + h; y++) console.log(String(y).padStart(3) + ' ' + Array.from(r.px.subarray(y * 240 + x0, y * 240 + x0 + w), v => v === BACK ? ' .. ' : v.toString(16).padStart(4, '0')).join(' '));
    break;
  }
  default: console.log('usage: crowd_prim.mjs table|sheet|gif|tiles|check [--out DIR]'); process.exit(2);
}
