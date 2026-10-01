// Pixels for docs/apps/derby-trig-cull-device.md: DERBY WATCH's oval under a
// held panning WIDE 2 with its unit at w -100 (now) and -3 (u 14), every 10 m
// of the leader over the bend, rendered by the real harness
// (tools/games/test_derby_host.c, built by run_derby.py: QuickJS, the plans'
// VM, the Kasane renderer). Also each run's frame statistics (frames.csv) for
// the largest draw and segment counts. WSL/Linux; run tools/games/run_derby.py
// once first (the binary and the lowered app; the app as it is in the tree).
//
//   node tools/games/ovalcost/w3_shots.mjs
// Writes .cache/trigcull_w3/<config>/<part>/*.ppm and docs/apps/derby-trig-cull-w3-*.png/gif.
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
const CACHE = path.join(ROOT, '.cache/trigcull_w3'), BIN = path.join(ROOT, '.cache/derby_host/asan/test-derby');
const LOWERED = path.join(ROOT, '.cache/derby_host/lowered/apps/derby');
// The harness keeps DERBY_LEADSHOTS in a 32-bit mask: two runs of 19.
const AT = [];
for (let m = 280; m <= 650; m += 10) AT.push(m);
const PARTS = [AT.filter((m, i) => i % 2 === 0), AT.filter((m, i) => i % 2 === 1)];
const CONFIGS = [{ name: 'w100', js: 'CAMS[8][5]=-100' }, { name: 'w3', js: 'CAMS[8][5]=-3' }];

function appDir() {
  const dir = path.join(CACHE, 'app/apps/derby');
  fs.rmSync(dir, { recursive: true, force: true });
  fs.mkdirSync(dir, { recursive: true });
  for (const f of fs.readdirSync(LOWERED)) fs.copyFileSync(path.join(LOWERED, f), path.join(dir, f));
  const w = fs.readFileSync(path.join(dir, 'derby_watch.js'), 'utf8');
  if (!/const OV = \[[\d.]+,/.test(w)) throw new Error('derby_watch.js: OV changed');
  fs.writeFileSync(path.join(dir, 'derby_watch.js'), w.replace(/const OV = \[[\d.]+,/, 'const OV = [1,'));
  return dir;
}

function shots(cfg, dir, k) {
  const out = path.join(CACHE, cfg.name, 'p' + k);
  fs.rmSync(out, { recursive: true, force: true });
  fs.mkdirSync(out, { recursive: true });
  const env = { ...process.env, DERBY_APP_DIR: dir, DERBY_TIER: '1', DERBY_NOCAM: '1', DERBY_PPM: out,
    DERBY_EACH: 'cam=8;man=9;', DERBY_LEADSHOTS: PARTS[k].map(m => `${m}:m${m}`).join(','), DERBY_CSV: path.join(out, 'frames.csv'),
    DERBY_JS: cfg.js, ASAN_OPTIONS: 'detect_leaks=0:abort_on_error=1', UBSAN_OPTIONS: 'halt_on_error=1' };
  const p = spawnSync(BIN, [], { cwd: ROOT, env, encoding: 'utf8', maxBuffer: 1 << 28 });
  // WIDE 2 held all game: the harness's own verdict fails on no VISION and no
  // feed frames (vis_frames, feed_frames). What matters here is in its lines.
  const ex = /exceptions=(\d+) bad_present=(\d+) failures=(\d+)/.exec(p.stdout);
  if (!ex || +ex[1] || +ex[2] || +ex[3]) { console.log(p.stdout.split('\n').slice(-30).join('\n'), p.stderr); throw new Error(`${cfg.name}: harness failed (${p.status})`); }
  for (const l of p.stdout.split('\n')) if (/^(limits used|panning units|frames \d+ \(procedural|screen:)/.test(l)) console.log(`  ${cfg.name} p${k}: ${l}`);
  const got = {};
  for (const f of fs.readdirSync(out)) { const m = /^derby_\d+_m(\d+)\.ppm$/.exec(f); if (m) got[+m[1]] = readPPM(path.join(out, f)); }
  if (Object.keys(got).length < PARTS[k].length) throw new Error(`${cfg.name}: ${Object.keys(got).length} of ${PARTS[k].length} panels`);
  return { got, csv: fs.readFileSync(path.join(out, 'frames.csv'), 'utf8'), stdout: p.stdout };
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

const dir = appDir(), R = {};
for (const cfg of CONFIGS) {
  R[cfg.name] = {};
  for (const k of [0, 1]) {
    const r = shots(cfg, dir, k);
    Object.assign(R[cfg.name], r.got);
    // frames.csv: the header names the columns; report the race frames' maxima.
    const [hd, ...rows] = r.csv.trim().split('\n'), H =hd.split(',');
    const max = {};
    for (const row of rows) row.split(',').forEach((v, i) => { const x = +v; if (!isNaN(x)) max[H[i]] = Math.max(max[H[i]] ?? -Infinity, x); });
    console.log(`${cfg.name} part ${k}: ${rows.length} frames; maxima ${H.filter(h => /draw|seg|line|step|cmd|newton|err/i.test(h)).map(h => `${h}=${max[h]}`).join(' ')}`);
    const fin = /^finish: (.*)$/m.exec(r.stdout);
    console.log(`  finish: ${fin ? fin[1] : '?'}`);
  }
}
const DOCS = path.join(ROOT, 'docs/apps');
// Sheet 1: w -3 over the bend every 20 m (rows of 4).
const B = AT.filter(m => m % 20 === 0), rows = [];
for (let i = 0; i < B.length; i += 4) rows.push(B.slice(i, i + 4).map(m => R.w3[m]));
png(path.join(DOCS, 'derby-trig-cull-w3-bend.png'), grid(rows));
// Sheet 2: w -100 beside w -3 where f sits on 200 and around it.
const PICK = [300, 400, 430, 440, 450, 460, 470, 480, 490, 500, 600];
png(path.join(DOCS, 'derby-trig-cull-w3-vs-w100.png'), grid(PICK.map(m => [R.w100[m], R.w3[m]])));
// GIF: every 10 m, w -100 | w -3.
gif(path.join(DOCS, 'derby-trig-cull-w3.gif'), AT.map(m => grid([[R.w100[m], R.w3[m]]])), 25);
