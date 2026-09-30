// q27: the crowd drawn half per frame (docs/kasane/derby-background-cost.md,
// "観客のチェッカーボード"). Host only, Node. Runs the crowd and stands plans
// through tools/kasane_ir/plan_js.mjs's float32 reference (the VM's rules)
// with the inputs course() (apps/derby/derby_view.js) computes, and:
//
//   check   for 3 tiers x 4 side cameras x camera positions along the course,
//           the two frames' dots (parity 0 and 1, same blink) against today's
//           plan: the union must be today's set of (x, y, colour), each dot
//           in exactly one frame.
//   sheet   one PNG: rows tier x {WIDE, FIELD}; columns today, even frame,
//           odd frame, the two averaged in linear light (a stand-in for the
//           eye's persistence), for the checkerboard and the dots-only form.
//   gif     8 consecutive frames (t = 0..7), today | checkerboard | dots
//           only, every tier x {WIDE, FIELD}; one with the camera still, one
//           panning at race pace (16 m/s, 30 fps).
//   variant FILE OUT   writes FILE (a derby_prog.js with the checkerboard
//           crowd) with the dots-only crowd instead (the host run and the
//           device copy of that form).
//
//   node tools/games/pancost/crowd_checker.mjs check|sheet|gif [--out DIR]
//
// The look is not judged here; this only hands the pictures over.
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import {fileURLToPath} from 'node:url';
import {findPlans, reference, compilePlan} from '../../kasane_ir/plan_js.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
const PROG = path.join(ROOT, 'apps/derby/derby_prog.js');

// Today's crowd (vm/main 604a3fd), verbatim.
const TODAY = `/** @plan crowd inputs: x0, spread, y0, rowGap, bays, sway, seed */
  crowd(p0, p1, p2, p3, p4, p5, p6) {
    let colour = seed * 12650 + 46496;
    const step = p6 * spread;
    let rowPhase = 0;
    let y = rowGap * .5 + y0;
    for (let j0 = 0; j0 < p1; j0++) {
      let x = x0;
      let phase = sway + rowPhase;
      for (let j1 = 0; j1 < bays; j1++) {
        for (let j2 = 0; j2 < p2; j2++) {
          plot(sin(phase) * 1.5 + x, y, colour);
          x += step;
          phase += 2.39996;
          colour = 105642 - colour;
        }
      }
      y += rowGap;
      rowPhase += .9;
    }
  },`;
// The dots-only form: every row starts at the frame's parity.
const DOTS = `/** @plan crowd inputs: x0, spread, y0, rowGap, bays, sway, seed, parity */
  crowd(p0, p1, p2, p3, p4, p5, p6, p7) {
    let dc = seed * 12650 + 46496;
    const step = p6 * spread;
    const step2 = step + step;
    const lim = bays * spread + x0 - step * .5;
    const xs = parity * step + x0;
    let ps = parity * 2.39996 + sway;
    let y = rowGap * .5 + y0;
    for (let j0 = 0; j0 < p1; j0++) {
      let x = xs;
      let phase = ps;
      for (let j1 = 0; j1 < bays * p7; j1++) {
        if (x > lim) break;
        plot(sin(phase) * 1.5 + x, y, dc);
        x += step2;
        phase += 4.79992;
      }
      for (let j2 = 0; j2 < (p7 + p7 - p2) * bays; j2++) {
        dc = 105642 - dc;
      }
      y += rowGap;
      ps += .9;
    }
  },`;

const src = fs.readFileSync(PROG, 'utf8');
const plansOf = text => new Map(findPlans(text, 'derby_prog.js').map(p => [p.name, p]));
const mine = plansOf(src);
const P = {today: plansOf(TODAY).get('crowd'), checker: mine.get('crowd'), dots: plansOf(DOTS).get('crowd'),
  stands: mine.get('stands')};
if (!P.checker.inputs.includes('parity')) throw new Error('apps/derby/derby_prog.js: crowd is not the q27 form');

// course() (derby_view.js), side cameras only; o = 120 (no screen feed).
const KN = [[3, 2, 3, 3, 8, 10], [4, 3, 4, 5, 5, 7], [5, 4, 6, 8, 4, 5]], TIERS = ['LIGHT', 'MID', 'HEAVY'];
const CAMS = {WIDE: [100, 9.7, 33], FIELD: [58, 15, 36], VISION: [130, 1.6, 84], FINISH: [170, 7, 22]};
const PI = Math.PI, flo = Math.floor;
function inputs(cam, tier, x0, t) {
  const [f, h, hy] = CAMS[cam], k = KN[tier], q = f / 40;
  const sx = w => 120 + (w - x0) * f / 40, gy = hy + h * f / 40, ty = e => hy + (h - e) * f / 40;
  const j = flo((x0 - 130 / q) / 12), a = sx(j * 12), dx = 12 * q;
  const n = Math.min(flo((700 - a) / dx), Math.ceil((250 - a) / dx) + 1);
  const sway = j * k[2] * 2.39996 % (2 * PI) - 2 * PI * Math.round(n * k[2] * .191);
  const args = k.concat(1 / k[2]), args2 = args.concat(Math.ceil(k[2] / 2));
  return {
    stands: [[a, dx, gy, -2.4 * q, n, 0, 0, ty(13.5)], args],
    today: [[a, dx, gy, -2.4 * q, n, sway, (t >> 3) & 1], args],
    half: [[a, dx, gy, -2.4 * q, n, sway, ((t >> 3) ^ t) & 1, t & 1], args2],
    window: [flo(ty(13.5) - 2 * 2.4 * q) - 2, Math.ceil(gy) + 3],
  };
}
const run = (plan, [inp, args]) => {
  const r = reference(plan, inp, args);
  if (r.status !== 'DONE') throw new Error(`${plan.name}: ${r.status} ${JSON.stringify(inp)}`);
  return r.seg;
};
const dots = (form, cam, tier, x0, t) => {
  const i = inputs(cam, tier, x0, t);
  return run(P[form], form === 'today' ? i.today : i.half);
};

function check() {
  let worst = 0;
  // The half forms add 2 * step and 2 * 2.39996 in float32 where today adds
  // step and 2.39996 twice, so a dot can round to the next pixel: counted
  // (same y and colour, x one off), anything else fails.
  console.log('tier cam form | positions | dots/frame today | even | odd | dots | 1 px off (float32) | other');
  for (const tier of [0, 1, 2]) for (const cam of Object.keys(CAMS)) for (const form of ['checker', 'dots']) {
    let pos = 0, nt = 0, ne = 0, no = 0, off1 = 0, other = 0;
    for (let x0 = 30; x0 < 2000; x0 += 1.37) for (const t of [0, 8]) {
      const key = ([x, y, , , c]) => `${x},${y},${c}`;
      const a = dots('today', cam, tier, x0, t), e = dots(form, cam, tier, x0, t), o = dots(form, cam, tier, x0, t + 1);
      const U = new Map();
      for (const s of e.concat(o)) U.set(key(s), (U.get(key(s)) || 0) + 1);
      const miss = [];
      for (const s of a) { const k = key(s); if (U.get(k)) U.set(k, U.get(k) - 1); else miss.push(s); }
      for (const [x, y, , , c] of miss) {
        const k = [x - 1, x + 1].map(v => `${v},${y},${c}`).find(k => U.get(k));
        if (k) { U.set(k, U.get(k) - 1); off1++; } else other++;
      }
      for (const v of U.values()) other += v;
      pos++; nt += a.length; ne += e.length; no += o.length;
    }
    worst += other;
    console.log(`${TIERS[tier]} ${cam} ${form} | ${pos} | ${(nt / pos).toFixed(1)} | ${(ne / pos).toFixed(1)} | ` +
      `${(no / pos).toFixed(1)} | ${nt} | ${off1} (${(100 * off1 / nt).toFixed(4)}%) | ${other}`);
  }
  console.log(worst ? 'CHECKER FAIL' : 'CHECKER PASS (the two frames are today\'s dots, each once, up to 1 px of float32 rounding)');
  const c = {checker: compilePlan(P.checker), dots: compilePlan(P.dots), today: compilePlan(P.today)};
  for (const [k, v] of Object.entries(c)) console.log(`${k}: ${v.count} instructions, ${v.plan.inputs.length} inputs`);
  return worst ? 1 : 0;
}

// ---- pictures
const BG = [0x18, 0x24, 0x1c];
const rgb565 = c => [(c >> 11 & 31) * 255 / 31 | 0, (c >> 5 & 63) * 255 / 63 | 0, (c & 31) * 255 / 31 | 0];
function raster(segs, y0, h) {
  const img = Array.from({length: h}, () => Array.from({length: 240}, () => BG));
  for (let [x0, ya, x1, yb, c] of segs) {
    const col = rgb565(c);
    let dx = Math.abs(x1 - x0), dy = -Math.abs(yb - ya), sx = x0 < x1 ? 1 : -1, sy = ya < yb ? 1 : -1, err = dx + dy;
    for (;;) {
      if (x0 >= 0 && x0 < 240 && ya - y0 >= 0 && ya - y0 < h) img[ya - y0][x0] = col;
      if (x0 === x1 && ya === yb) break;
      const e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; ya += sy; }
    }
  }
  return img;
}
function frame(form, cam, tier, x0, t) {
  const i = inputs(cam, tier, x0, t), [y0, y1] = i.window;
  return raster(run(P.stands, i.stands).concat(dots(form, cam, tier, x0, t)), y0, y1 - y0);
}
const lin = v => (v /= 255) <= .04045 ? v / 12.92 : ((v + .055) / 1.055) ** 2.4;
const enc = v => Math.round(255 * (v <= .0031308 ? v * 12.92 : 1.055 * v ** (1 / 2.4) - .055));
const average = (a, b) => a.map((r, y) => r.map((p, x) => p.map((v, k) => enc((lin(v) + lin(b[y][x][k])) / 2))));
const hcat = (panels, gap = 4, col = [255, 255, 255]) =>
  panels[0].map((_, y) => panels.flatMap((p, i) => (i ? Array(gap).fill(col) : []).concat(p[y])));
const vcat = (panels, gap = 2, col = [255, 255, 255]) =>
  panels.flatMap((p, i) => (i ? Array.from({length: gap}, () => Array(p[0].length).fill(col)) : []).concat(p));
const scale = (img, s) => img.flatMap(r => Array(s).fill(r.flatMap(p => Array(s).fill(p))));
const pad = (img, w) => img.map(r => r.concat(Array(w - r.length).fill([0, 0, 0])));

function png(file, rows) {
  const raw = Buffer.concat(rows.map(r => Buffer.from([0, ...r.flat()])));
  const chunk = (t, d) => {
    const b = Buffer.alloc(8 + d.length + 4);
    b.writeUInt32BE(d.length, 0); b.write(t, 4); d.copy(b, 8);
    b.writeUInt32BE(zlib.crc32(Buffer.concat([Buffer.from(t), d])) >>> 0, 8 + d.length);
    return b;
  };
  const ih = Buffer.alloc(13);
  ih.writeUInt32BE(rows[0].length, 0); ih.writeUInt32BE(rows.length, 4); ih[8] = 8; ih[9] = 2;
  fs.mkdirSync(path.dirname(file), {recursive: true});
  fs.writeFileSync(file, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ih),
    chunk('IDAT', zlib.deflateSync(raw, {level: 9})), chunk('IEND', Buffer.alloc(0))]));
  console.log('wrote', path.relative(ROOT, file), `${rows[0].length}x${rows.length}`);
}

// Scenes: the race's middle (x0 = 700 m, blink 0).
const SCENES = [0, 1, 2].flatMap(tier => ['WIDE', 'FIELD'].map(cam => ({tier, cam})));
const X0 = 700;
function sheet(out) {
  for (const form of ['checker', 'dots']) {
    const rows = SCENES.map(({tier, cam}) => {
      const a = frame('today', cam, tier, X0, 0), e = frame(form, cam, tier, X0, 0), o = frame(form, cam, tier, X0, 1);
      return hcat([a, e, o, average(e, o)]);
    });
    png(path.join(out, `derby-crowd-${form}-preview.png`), scale(vcat(rows, 3), 3));
  }
  console.log(`rows: ${SCENES.map(s => TIERS[s.tier] + ' ' + s.cam).join(', ')}; columns: today, even frame, odd frame, average (linear light); x0 = ${X0} m, blink 0, x3`);
}

// ---- GIF (LZW, one global palette)
function gif(file, frames, delayCs) {
  const pal = new Map(), key = p => p.join(',');
  for (const f of frames) for (const r of f) for (const p of r) if (!pal.has(key(p))) pal.set(key(p), pal.size);
  if (pal.size > 256) throw new Error('more than 256 colours');
  const bits = Math.max(2, Math.ceil(Math.log2(pal.size))), w = frames[0][0].length, h = frames[0].length;
  const out = [Buffer.from('GIF89a')], u16 = v => [v & 255, v >> 8];
  out.push(Buffer.from([...u16(w), ...u16(h), 0xf0 | (bits - 1), 0, 0]));
  const ct = Buffer.alloc(3 << bits);
  for (const [k, i] of pal) k.split(',').forEach((v, j) => { ct[i * 3 + j] = +v; });
  out.push(ct, Buffer.from([0x21, 0xff, 11, ...Buffer.from('NETSCAPE2.0'), 3, 1, 0, 0, 0]));
  for (const f of frames) {
    out.push(Buffer.from([0x21, 0xf9, 4, 0, ...u16(delayCs), 0, 0, 0x2c, 0, 0, 0, 0, ...u16(w), ...u16(h), 0, bits]));
    const idx = f.flatMap(r => r.map(p => pal.get(key(p))));
    const clear = 1 << bits, eoi = clear + 1, bytes = [];
    let size = bits + 1, next = eoi + 1, dict = new Map(), acc = 0, n = 0;
    const put = c => { acc |= c << n; n += size; while (n >= 8) { bytes.push(acc & 255); acc >>>= 8; n -= 8; } };
    put(clear);
    let cur = idx[0];
    for (let i = 1; i < idx.length; i++) {
      const k = cur * 4096 + idx[i];
      if (dict.has(k)) { cur = dict.get(k); continue; }
      put(cur);
      if (next < 4096) { dict.set(k, next++); if (next > (1 << size) && size < 12) size++; }
      else { put(clear); dict = new Map(); size = bits + 1; next = eoi + 1; }
      cur = idx[i];
    }
    put(cur); put(eoi);
    if (n) bytes.push(acc & 255);
    const blocks = [];
    for (let i = 0; i < bytes.length; i += 255) blocks.push(Math.min(255, bytes.length - i), ...bytes.slice(i, i + 255));
    out.push(Buffer.from([...blocks, 0]));
  }
  out.push(Buffer.from([0x3b]));
  fs.mkdirSync(path.dirname(file), {recursive: true});
  fs.writeFileSync(file, Buffer.concat(out));
  console.log('wrote', path.relative(ROOT, file), `${w}x${h}, ${frames.length} frames, ${delayCs * 10} ms`);
}
function gifs(out) {
  for (const [name, speed] of [['still', 0], ['pan', 16 / 30]]) {
    const frames = [];
    for (let t = 0; t < 8; t++) {
      const x0 = X0 + speed * t;
      const rows = SCENES.map(({tier, cam}) => {
        const cols = ['today', 'checker', 'dots'].map(f => frame(f, cam, tier, x0, t));
        return hcat(cols);
      });
      const img = vcat(rows, 3);
      frames.push(scale(pad(img, img[0].length), 2));
    }
    // 30 fps is 3.33 cs; GIF delays are whole centiseconds (3 = 33 ms).
    gif(path.join(out, `derby-crowd-${name}.gif`), frames, 3);
  }
  console.log(`rows: ${SCENES.map(s => TIERS[s.tier] + ' ' + s.cam).join(', ')}; columns: today, checkerboard, dots only; t = 0..7 from x0 = ${X0} m, x2`);
}

const argv = process.argv.slice(2), outDir = argv.includes('--out') ? path.resolve(argv[argv.indexOf('--out') + 1]) : path.join(ROOT, 'docs/apps');
export {dots, inputs, TIERS, CAMS};
if (!process.env.NOMAIN) switch (argv[0]) {
  case 'check': process.exit(check());
  case 'sheet': sheet(outDir); break;
  case 'gif': gifs(outDir); break;
  case 'variant': {
    const text = fs.readFileSync(argv[1], 'utf8'), p = findPlans(text).find(p => p.name === 'crowd');
    fs.writeFileSync(argv[2], text.slice(0, p.start) + DOTS + text.slice(p.end + 1));
    console.log('wrote', argv[2]);
    break;
  }
  default: console.log('usage: crowd_checker.mjs check|sheet|gif [--out DIR] | variant FILE OUT'); process.exit(2);
}
