// Host-only coverage comparison. No frame RNG, native/runtime edits or device claims.
'use strict';
const fs = require('node:fs'), path = require('node:path'), zlib = require('node:zlib'), assert = require('node:assert/strict');
const SIZE = 8, METHODS = ['smooth', 'stipple', 'comic-tone', 'hatch', 'crosshatch'];
function modulo(value, period) { return (value % period + period) % period; }
function bayer(x, y) {
  let rank = 0;
  for (let bit = 0; bit < 3; ++bit) {
    const xx = x >> bit & 1, yy = y >> bit & 1;
    rank += ((xx ^ yy) * 2 + yy) * (1 << (4 - bit * 2));
  }
  return rank;
}
function rankTable(score) {
  const points = [];
  for (let y = 0; y < SIZE; ++y) for (let x = 0; x < SIZE; ++x) points.push({x, y, score: score(x, y), tie: bayer(x, y)});
  points.sort((a, b) => a.score - b.score || a.tie - b.tie);
  const rank = new Uint8Array(SIZE * SIZE);
  for (let i = 0; i < points.length; ++i) rank[points[i].y * SIZE + points[i].x] = i;
  return rank;
}
const tables = {
  stipple: rankTable((x, y) => bayer(x, y)),
  'comic-tone': rankTable((x, y) => (x - 3.5) ** 2 + (y - 3.5) ** 2),
  hatch: rankTable((x, y) => Math.abs(modulo(x + y + 4, SIZE) - 4)),
  crosshatch: rankTable((x, y) => Math.min(Math.abs(modulo(x + y + 4, SIZE) - 4), Math.abs(modulo(x - y + 4, SIZE) - 4)))
};
function coverage(method, value, x, y, phase) {
  value = Math.max(0, Math.min(1, value));
  if (method === 'smooth') return value;
  if (!tables[method]) throw Error('unknown coverage method');
  const xx = modulo(Math.floor(x + phase[0]), SIZE), yy = modulo(Math.floor(y + phase[1]), SIZE);
  return value > (tables[method][yy * SIZE + xx] + 0.5) / 64 ? 1 : 0;
}
function checks() {
  const phase = [3, 5];
  for (const method of METHODS) {
    let previous = -1;
    for (let level = 0; level <= 64; ++level) {
      let mean = 0;
      for (let y = 0; y < 8; ++y) for (let x = 0; x < 8; ++x) {
        const a = coverage(method, level / 64, x, y, phase), b = coverage(method, level / 64, x, y, phase);
        assert.equal(a, b, 'identical input/phase repeats exactly'); mean += a / 64;
        if (level) assert.ok(a >= coverage(method, (level - 1) / 64, x, y, phase), 'light-derived scalar changes monotonically');
      }
      assert.ok(mean >= previous, 'mean tone monotone');
      assert.ok(Math.abs(mean - level / 64) < 1e-12, 'full deterministic tile preserves mean coverage');
      previous = mean;
    }
    for (let y = -8; y < 8; ++y) for (let x = -8; x < 8; ++x)
      assert.equal(coverage(method, 0.37, x, y, phase), coverage(method, 0.37, x + 8, y + 8, phase), 'stable periodic phase');
  }
}
const W = 160, H = 104, SKY = [110, 157, 196], CLOUD = [247, 251, 255];
function field(x, y, cx, squash, light) {
  const u = (x - cx) / (52 / Math.sqrt(squash)), v = (y - 54) / (35 * squash), r2 = u * u + v * v;
  if (r2 >= 1) return 0;
  const edge = Math.min(1, (1 - r2) / 0.3), tone = Math.max(0.05, Math.min(1, light + 0.2 * u + 0.13 * Math.sqrt(1 - r2)));
  return edge * tone;
}
function synthetic(method, anchor, cx = 80, squash = 1, light = 0.65) {
  const rgb = Buffer.alloc(W * H * 3), mask = new Float32Array(W * H);
  for (let y = 0; y < H; ++y) for (let x = 0; x < W; ++x) {
    const px = anchor === 'body' ? (x - cx) * Math.sqrt(squash) + 80 : x,
      py = anchor === 'body' ? (y - 54) / squash + 54 : y,
      q = coverage(method, field(x, y, cx, squash, light), px, py, [3, 5]), p = (y * W + x) * 3;
    mask[y * W + x] = q;
    for (let c = 0; c < 3; ++c) rgb[p + c] = Math.round(SKY[c] * (1 - q) + CLOUD[c] * q);
  }
  return {rgb, mask};
}
function difference(a, b) { let n = 0; for (let i = 0; i < a.length; ++i) if (Math.abs(a[i] - b[i]) > 1e-6) ++n; return n; }
function temporal() {
  const report = [];
  for (const method of METHODS) for (const anchor of ['screen', 'body']) {
    let translationChanged = 0, squashChanged = 0, previous = synthetic(method, anchor).mask;
    for (let i = 1; i <= 32; ++i) {
      const next = synthetic(method, anchor, 80 + i / 4).mask;
      translationChanged += difference(previous, next); previous = next;
    }
    previous = synthetic(method, anchor).mask;
    for (let i = 1; i <= 32; ++i) {
      const next = synthetic(method, anchor, 80, 1 + i / 160).mask;
      squashChanged += difference(previous, next); previous = next;
    }
    const held = synthetic(method, anchor, 80, 1.2).mask;
    assert.equal(difference(previous, held), 0, 'motion stop has no residual flicker');
    report.push({method, anchor, translationChangedPixelsSum: translationChanged, squashChangedPixelsSum: squashChanged, holdChangedPixels: 0});
  }
  return report;
}
function crc(bytes) { let v = 0xffffffff; for (const x of bytes) { v ^= x; for (let i = 0; i < 8; ++i) v = v >>> 1 ^ ((v & 1) ? 0xedb88320 : 0); } return (v ^ 0xffffffff) >>> 0; }
function chunk(tag, data) { const type = Buffer.from(tag), size = Buffer.alloc(4), checksum = Buffer.alloc(4); size.writeUInt32BE(data.length); checksum.writeUInt32BE(crc(Buffer.concat([type, data]))); return Buffer.concat([size, type, data, checksum]); }
function png(file, width, height, rgb) {
  const ihdr = Buffer.alloc(13), scan = Buffer.alloc((width * 3 + 1) * height);
  ihdr.writeUInt32BE(width); ihdr.writeUInt32BE(height, 4); ihdr[8] = 8; ihdr[9] = 2;
  for (let y = 0; y < height; ++y) rgb.copy(scan, y * (width * 3 + 1) + 1, y * width * 3, (y + 1) * width * 3);
  fs.writeFileSync(file, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(scan)), chunk('IEND', Buffer.alloc(0))]));
}
function sheet(file, images, width, height) {
  const output = Buffer.alloc(images.length * width * height * 3);
  for (let y = 0; y < height; ++y) for (let i = 0; i < images.length; ++i)
    images[i].copy(output, y * images.length * width * 3 + i * width * 3, y * width * 3, (y + 1) * width * 3);
  png(file, width * images.length, height, output);
}
function ppm(file) {
  const data = fs.readFileSync(file), match = /^P6\s+(\d+)\s+(\d+)\s+255\s/.exec(data.toString('ascii', 0, 100));
  if (!match) throw Error('expected simple RGB P6 PPM');
  const width = +match[1], height = +match[2], rgb = data.subarray(match[0].length);
  if (rgb.length !== width * height * 3) throw Error('PPM byte count');
  return {width, height, rgb};
}
function main() {
  checks();
  const outIndex = process.argv.indexOf('--out'), inputIndex = process.argv.indexOf('--input'),
    out = path.resolve(outIndex < 0 ? '.cache/character-coverage-study' : process.argv[outIndex + 1]);
  fs.mkdirSync(out, {recursive: true});
  sheet(path.join(out, 'coverage-comparison.png'), METHODS.map(m => synthetic(m, 'screen').rgb), W, H);
  sheet(path.join(out, 'body-phase-comparison.png'), METHODS.map(m => synthetic(m, 'body', 83.75, 1.12).rgb), W, H);
  const report = {status: 'host-only unadopted comparison; no native/device result',
    source: 'controlled scalar coverage field after a hypothetical light/material evaluation, not a rendered cloud model',
    columns: METHODS, tileSize: 8, phase: [3, 5], temporal: temporal(),
    interpretation: 'changed-pixel sums include legitimate motion and subpixel sampling; they do not isolate perceptual shimmer or prove absence of moire'};
  if (inputIndex >= 0) {
    const file = path.resolve(process.argv[inputIndex + 1]), capture = ppm(file), images = [capture.rgb];
    for (const method of METHODS.slice(1)) {
      const output = Buffer.alloc(capture.rgb.length);
      for (let y = 0; y < capture.height; ++y) for (let x = 0; x < capture.width; ++x) {
        const p = (y * capture.width + x) * 3,
          luminance = (0.2126 * capture.rgb[p] + 0.7152 * capture.rgb[p + 1] + 0.0722 * capture.rgb[p + 2]) / 255,
          q = coverage(method, luminance, x, y, [3, 5]);
        output[p] = output[p + 1] = output[p + 2] = q * 255;
      }
      images.push(output);
    }
    sheet(path.join(out, 'captured-luminance-illustration.png'), images, capture.width, capture.height);
    report.capture = {file, width: capture.width, height: capture.height,
      caveat: 'composited screenshot luminance only; cannot recover true alpha, light, albedo or surface masks; this is not a production shader simulation'};
  }
  fs.writeFileSync(path.join(out, 'report.json'), JSON.stringify(report, null, 2));
  fs.writeFileSync(path.join(out, 'index.html'), '<!doctype html><meta charset="utf-8"><title>Coverage study (host only)</title><style>body{font:16px sans-serif;background:#edf3f8;color:#26384b;max-width:1100px;margin:32px auto}img{max-width:100%;image-rendering:pixelated}code{color:#305f83}</style><h1>Coverage study — not adopted</h1><p>Columns: ' + METHODS.join(' / ') + '</p><p>Controlled scalar post-lighting coverage, not a final character render. Fixed deterministic 8×8 phase; no frame random.</p><img src="coverage-comparison.png"><p>Body-coordinate phase under translation and squash:</p><img src="body-phase-comparison.png">' + (report.capture ? '<p>Native captured image followed by luminance-only illustrations. No alpha/albedo recovery:</p><img src="captured-luminance-illustration.png">' : '') + '<p>See report.json for deterministic tests and raw transition counts; those counts include legitimate motion and are not a perceptual pass.</p>');
  console.log('COVERAGE_STUDY PASS: deterministic phase, full-tile tone/monotonicity, no residual flicker at stop; host only, unadopted');
}
if (require.main === module) main();
module.exports = {coverage, checks, temporal};
