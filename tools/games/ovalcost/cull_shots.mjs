// Pixels for docs/apps/derby-trig-cull.md: DERBY WATCH's oval under a held
// panning WIDE 2, rendered by the real harness (tools/games/test_derby_host.c,
// built by run_derby.py: QuickJS, the plans' VM, the Kasane renderer) for the
// app as it is and for each variant of variants.mjs, then compared pixel for
// pixel. WSL/Linux; run tools/games/run_derby.py once first (the binary and
// the lowered app).
//
//   node tools/games/ovalcost/cull_shots.mjs [variant ...]
// Writes .cache/trigcull/<config>/<variant>/*.ppm, prints a table of the
// pixels that differ from the app's, and docs/apps/derby-trig-cull-*.png/gif.
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import { execFileSync, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { patchApp } from './variants.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
const CACHE = path.join(ROOT, '.cache/trigcull'), BIN = path.join(ROOT, '.cache/derby_host/asan/test-derby');
const LOWERED = path.join(ROOT, '.cache/derby_host/lowered/apps/derby');
const args = process.argv.slice(2);
const VARIANTS = args.length ? args : ['vetab', 'cull4', 'merge1', 'merge2'];
// The four panels of the sheet, and 28 points of the bend for the table (the
// harness keeps DERBY_LEADSHOTS in a 32-bit mask: at most 32).
const SHOW = [300, 400, 500, 600], AT = [];
for (let m = 280; m <= 640; m += 20) AT.push(m);
for (let m = 290; m <= 630; m += 40) AT.push(m);
AT.sort((a, b) => a - b);
const CONFIGS = [
  { name: 'w100', label: 'WIDE 2 の台 w=−100（今）', js: '', course: 'oval' },
  { name: 'u14', label: 'WIDE 2 の台 u=14（w=−14）', js: 'CAMS[8][5]=-14', course: 'oval' },
  { name: 'straight', label: '直線コース WIDE 2', js: '', course: 'straight', at: [150, 250, 350, 450, 550, 650, 750, 850] },
];

function appDir(cfg, v) {
  const dir = path.join(CACHE, cfg.name, v || 'app', 'apps/derby');
  fs.rmSync(dir, { recursive: true, force: true });
  fs.mkdirSync(dir, { recursive: true });
  for (const f of fs.readdirSync(LOWERED)) fs.copyFileSync(path.join(LOWERED, f), path.join(dir, f));
  const rd = f => fs.readFileSync(path.join(dir, f), 'utf8');
  const files = patchApp({ view: rd('derby_view.js'), pan: rd('derby_pan.js'), scene: rd('derby_scene.js') }, v);
  fs.writeFileSync(path.join(dir, 'derby_view.js'), files.view);
  fs.writeFileSync(path.join(dir, 'derby_pan.js'), files.pan);
  fs.writeFileSync(path.join(dir, 'derby_scene.js'), files.scene);
  const w = rd('derby_watch.js');
  // run_derby.py --course leaves its lowered copy forced; set it either way.
  if (!/const OV = \[[\d.]+,/.test(w)) throw new Error('derby_watch.js: OV changed');
  fs.writeFileSync(path.join(dir, 'derby_watch.js'), w.replace(/const OV = \[[\d.]+,/, `const OV = [${cfg.course === 'oval' ? 1 : 0},`));
  return dir;
}

function shots(cfg, v) {
  const dir = appDir(cfg, v), out = path.join(CACHE, cfg.name, v || 'app', 'ppm');
  fs.rmSync(out, { recursive: true, force: true });
  fs.mkdirSync(out, { recursive: true });
  const at = cfg.at || AT;
  const env = { ...process.env, DERBY_APP_DIR: dir, DERBY_TIER: '1', DERBY_NOCAM: '1', DERBY_PPM: out,
    DERBY_EACH: 'cam=8;man=9;', DERBY_LEADSHOTS: at.map(m => `${m}:m${m}`).join(','), DERBY_CSV: path.join(out, 'frames.csv'),
    ASAN_OPTIONS: 'detect_leaks=0:abort_on_error=1', UBSAN_OPTIONS: 'halt_on_error=1' };
  if (cfg.js) env.DERBY_JS = cfg.js;
  const p = spawnSync(BIN, [], { cwd: ROOT, env, encoding: 'utf8', maxBuffer: 1 << 28 });
  const got = {};
  for (const f of fs.readdirSync(out)) { const m = /^derby_\d+_m(\d+)\.ppm$/.exec(f); if (m) got[+m[1]] = readPPM(path.join(out, f)); }
  if (Object.keys(got).length < at.length) {
    console.log(p.stdout.split('\n').slice(-20).join('\n'), p.stderr);
    throw new Error(`${cfg.name}/${v}: ${Object.keys(got).length} of ${at.length} panels`);
  }
  return got;
}

function readPPM(f) {
  const b = fs.readFileSync(f), h = 'P6\n240 135\n255\n';
  if (b.toString('latin1', 0, h.length) !== h) throw new Error('ppm header ' + f);
  return { w: 240, h: 135, d: new Uint8Array(b.subarray(h.length)) };
}
const img = (w, h, c = [0x30, 0x30, 0x30]) => { const d = new Uint8Array(w * h * 3); for (let i = 0; i < w * h; i++) d.set(c, i * 3); return { w, h, d }; };
function diff(a, b) {
  const o = img(240, 135, [0, 0, 0]); let n = 0;
  for (let i = 0; i < 240 * 135; i++) {
    const s = a.d[3 * i] !== b.d[3 * i] || a.d[3 * i + 1] !== b.d[3 * i + 1] || a.d[3 * i + 2] !== b.d[3 * i + 2];
    if (s) { ++n; o.d.set([255, 0, 255], 3 * i); } else { const g = (a.d[3 * i] + a.d[3 * i + 1] + a.d[3 * i + 2]) / 12 | 0; o.d.set([g, g, g], 3 * i); }
  }
  return { n, img: o };
}
function blit(dst, src, x, y) { for (let r = 0; r < src.h; r++) dst.d.set(src.d.subarray(r * src.w * 3, (r + 1) * src.w * 3), ((y + r) * dst.w + x) * 3); }
function grid(cells, gap = 2) {
  const R = cells.length, C = Math.max(...cells.map(r => r.length));
  const out = img(C * 240 + (C - 1) * gap, R * 135 + (R - 1) * gap);
  cells.forEach((r, i) => r.forEach((c, j) => c && blit(out, c, j * (240 + gap), i * (135 + gap))));
  return out;
}
function png(file, I) {
  const raw = Buffer.alloc((I.w * 3 + 1) * I.h);
  for (let y = 0; y < I.h; y++) Buffer.from(I.d.buffer, I.d.byteOffset + y * I.w * 3, I.w * 3).copy(raw, y * (I.w * 3 + 1) + 1);
  const chunk = (t, d) => { const b = Buffer.alloc(12 + d.length); b.writeUInt32BE(d.length, 0); b.write(t, 4); d.copy(b, 8); b.writeUInt32BE(zlib.crc32(Buffer.concat([Buffer.from(t), d])) >>> 0, 8 + d.length); return b; };
  const ih = Buffer.alloc(13); ih.writeUInt32BE(I.w, 0); ih.writeUInt32BE(I.h, 4); ih[8] = 8; ih[9] = 2;
  fs.writeFileSync(file, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ih), chunk('IDAT', zlib.deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0))]));
  console.log('wrote', path.relative(ROOT, file), `${I.w}x${I.h}`);
}
// GIF89a, global palette (the panels use few colours), LZW (crowd_look.mjs's).
function gif(file, frames, delayCs) {
  const pal = new Map();
  for (const f of frames) for (let i = 0; i < f.d.length; i += 3) { const k = f.d[i] << 16 | f.d[i + 1] << 8 | f.d[i + 2]; if (!pal.has(k)) pal.set(k, pal.size); }
  if (pal.size > 256) throw new Error('more than 256 colours');
  const bits = Math.max(2, Math.ceil(Math.log2(pal.size))), w = frames[0].w, h = frames[0].h, u16 = v => [v & 255, v >> 8];
  const out = [Buffer.from('GIF89a'), Buffer.from([...u16(w), ...u16(h), 0xf0 | (bits - 1), 0, 0])];
  const ct = Buffer.alloc(3 << bits);
  for (const [k, i] of pal) { ct[i * 3] = k >> 16; ct[i * 3 + 1] = k >> 8 & 255; ct[i * 3 + 2] = k & 255; }
  out.push(ct, Buffer.from([0x21, 0xff, 11, ...Buffer.from('NETSCAPE2.0'), 3, 1, 0, 0, 0]));
  for (const f of frames) {
    out.push(Buffer.from([0x21, 0xf9, 4, 0, ...u16(delayCs), 0, 0, 0x2c, 0, 0, 0, 0, ...u16(w), ...u16(h), 0, bits]));
    const n = w * h, idx = new Uint16Array(n);
    for (let i = 0; i < n; i++) idx[i] = pal.get(f.d[i * 3] << 16 | f.d[i * 3 + 1] << 8 | f.d[i * 3 + 2]);
    const clear = 1 << bits, eoi = clear + 1, bytes = [];
    let size = bits + 1, next = eoi + 1, dict = new Map(), acc = 0, nb = 0;
    const put = c => { acc |= c << nb; nb += size; while (nb >= 8) { bytes.push(acc & 255); acc >>>= 8; nb -= 8; } };
    put(clear);
    let cur = idx[0];
    for (let i = 1; i < n; i++) {
      const key = cur * 4096 + idx[i];
      if (dict.has(key)) { cur = dict.get(key); continue; }
      put(cur);
      if (next < 4096) { dict.set(key, next++); if (next > (1 << size) && size < 12) size++; }
      else { put(clear); dict = new Map(); size = bits + 1; next = eoi + 1; }
      cur = idx[i];
    }
    put(cur); put(eoi);
    if (nb) bytes.push(acc & 255);
    const blocks = [];
    for (let i = 0; i < bytes.length; i += 255) blocks.push(Math.min(255, bytes.length - i), ...bytes.slice(i, i + 255));
    out.push(Buffer.from([...blocks, 0]));
  }
  out.push(Buffer.from([0x3b]));
  fs.writeFileSync(file, Buffer.concat(out));
  console.log('wrote', path.relative(ROOT, file), `${w}x${h}, ${frames.length} frames`);
}

const R = {};
for (const cfg of CONFIGS) {
  R[cfg.name] = { app: shots(cfg, null) };
  for (const v of VARIANTS) R[cfg.name][v] = shots(cfg, v);
  const at = cfg.at || AT;
  console.log(`\n${cfg.label}: 先頭 ${at[0]}..${at[at.length - 1]} m の ${at.length} 枚、今のアプリとの差（画素の割合: 平均 / 最大）`);
  for (const v of VARIANTS) {
    const fr = at.map(m => diff(R[cfg.name].app[m], R[cfg.name][v][m]).n / (240 * 135));
    console.log(`  ${v}: ${(100 * fr.reduce((a, b) => a + b, 0) / fr.length).toFixed(3)} % / ${(100 * Math.max(...fr)).toFixed(3)} %` +
      (cfg.name === 'straight' ? '' : `  (300/400/500/600 m: ${SHOW.map(m => (100 * diff(R[cfg.name].app[m], R[cfg.name][v][m]).n / (240 * 135)).toFixed(2)).join(' / ')} %)`));
  }
}
// Sheets: rows the leader at 300..600 m; columns the app (w -100), u 14, then
// for each culling variant shown, its panel and its difference from the app
// (magenta), for u 14.
const DOCS = path.join(ROOT, 'docs/apps');
const SHEET_V = VARIANTS.filter(v => v !== 'vetab');
png(path.join(DOCS, 'derby-trig-cull-camera.png'), grid(SHOW.map(m => [R.w100.app[m], R.u14.app[m]])));
for (const cfg of ['w100', 'u14'])
  png(path.join(DOCS, `derby-trig-cull-${cfg}.png`), grid(SHOW.map(m => [R[cfg].app[m], ...SHEET_V.flatMap(v => [R[cfg][v][m], diff(R[cfg].app[m], R[cfg][v][m]).img])])));
// GIFs: every 10 m of the bend, the app beside each variant, for u 14 and w -100.
for (const cfg of ['w100', 'u14'])
  gif(path.join(DOCS, `derby-trig-cull-${cfg}.gif`), AT.map(m => grid([[R[cfg].app[m], ...SHEET_V.map(v => R[cfg][v][m])]])), 25);
