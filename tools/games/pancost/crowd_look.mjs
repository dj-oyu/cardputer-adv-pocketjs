// Crowd look candidates for DERBY WATCH (docs/kasane/derby-background-cost.md,
// "観客の見え方の候補"). Host only, Node, no device. Every candidate is a pair
// of @plan functions (side view and panning view) generated here as text,
// compiled by tools/kasane_ir/plan_js.mjs (instruction and register counts
// are the compiler's) and run by an IR stepper with ksn_proc_step's rules
// (main/ui/kasane/ksn_procedural.c: one step per executed instruction, float32
// registers), which gives the segments drawn and the steps taken. The stepper
// is checked against plan_js.mjs's float32 reference on every draw.
//
//   node tools/games/pancost/crowd_look.mjs table          costs per scene (stdout)
//   node tools/games/pancost/crowd_look.mjs sheet [--out DIR]   one PNG, x3
//   node tools/games/pancost/crowd_look.mjs gif [--out DIR]     8 frames a candidate
//   node tools/games/pancost/crowd_look.mjs interp         linear-in-bay error (pan)
//   --set look|bprime   the candidates shown (look: A4..F, the default;
//                       bprime: B and B'1..3, derby-crowd-bprime-*)
//
// Scenes: LIGHT/MID/HEAVY x {side WIDE, panning WIDE} x leader at 300 / 800 m.
// The panning unit is forced (the game takes it only past 60 m from a unit).
// The background is today's stands (side: stands; pan: prail at 40 m and the
// tier lines, hl) on the host sheets' ground colour. The look is not judged
// here; this only hands the pictures and the counts over.
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import {fileURLToPath} from 'node:url';
import {findPlans, reference, compilePlan} from '../../kasane_ir/plan_js.mjs';
import {assemble, stats} from '../../kasane_ir/kir.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
// Today's plans as they were before the pattern line (the app's crowd is
// now P24: crowd_p24.mjs).
const PROG = fs.readFileSync(path.join(ROOT, 'tools/games/pancost/crowd_today_plans.js'), 'utf8');
const plansOf = text => new Map(findPlans(text, 'gen').map(p => [p.name, p]));
const TODAY = plansOf(PROG);
const KN = [[3, 2, 3, 3, 8, 10], [4, 3, 4, 5, 5, 7], [5, 4, 6, 8, 4, 5]], TIERS = ['LIGHT', 'MID', 'HEAVY'];
const PI = Math.PI, flo = Math.floor, rnd = Math.round;

// ------------------------------------------------------------ the stepper
// ksn_proc_step on assembled rows [op, dst, a, b, value, color].
function runIR(rows, input) {
  const reg = new Float32Array(16), seg = [], loops = [], end = {}, st = [];
  rows.forEach((r, i) => { if (r[0] === 5 || r[0] === 10) st.push(i); else if (r[0] === 6) end[st.pop()] = i; });
  let pc = 0, steps = 0, sins = 0, pen = null, raster = 0;
  const co = v => { if (!Number.isFinite(v) || v < -480 || v > 720) throw new Error('INVALID coord ' + v); return Math.sign(v) * flo(Math.abs(v) + .5); };
  const emit = (x0, y0, x1, y1, c) => {
    if (seg.length === 1024) throw new Error('LIMIT segments');
    const cost = Math.max(Math.abs(x1 - x0), Math.abs(y1 - y0)) + 1;
    if ((raster += cost) > 8192) throw new Error('LIMIT raster');
    seg.push([x0, y0, x1, y1, c]);
  };
  while (pc < rows.length) {
    if (steps === 10000) throw new Error('LIMIT steps');
    const [op, dst, a, b, v, c] = rows[pc], last = pc++;
    steps++;
    switch (op) {
      case 0: reg[dst] = v; break;
      case 1: reg[dst] = input[a]; break;
      case 2: reg[dst] = reg[a] + reg[b]; break;
      case 3: reg[dst] = reg[a] * reg[b]; break;
      case 4: reg[dst] = Math.sin(reg[a]); sins++; break;
      case 5: case 10: {
        const n = op === 5 ? a : reg[a];
        if (!Number.isInteger(n) || n < 0 || n > 255) throw new Error('INVALID count ' + n);
        if (!n) { pc = end[last] + 1; break; }
        loops.push({pc: last, rem: n}); break;
      }
      case 6: { const L = loops[loops.length - 1]; if (--L.rem) pc = L.pc + 1; else loops.pop(); break; }
      case 11: if (reg[a] > reg[b]) { pc = end[loops.pop().pc] + 1; } break;
      case 7: case 8: case 9: case 12: case 13: {
        const col = op >= 12 ? reg[dst] : c;
        if (!Number.isInteger(col) || col < 0 || col > 65535) throw new Error('INVALID colour ' + col);
        const x = co(reg[a]), y = co(reg[b]);
        if (op === 8 || op === 12) emit(x, y, x, y, col);
        if ((op === 9 || op === 13) && pen) emit(pen[0], pen[1], x, y, col);
        pen = [x, y]; break;
      }
      case 14: {
        const px = [0, 1, 2, 3].map(p => reg[dst + 2 * p]), py = [0, 1, 2, 3].map(p => reg[dst + 2 * p + 1]);
        const F = Math.fround;
        let X = co(px[0]), Y = co(py[0]);
        for (let s = 1; s <= a; s++) {
          const t = F(F(s) / F(a)), u = F(1 - t);
          const bz = q => F(F(F(F(F(F(u * u) * u) * q[0]) + F(F(F(F(3 * u) * u) * t) * q[1])) + F(F(F(F(3 * u) * t) * t) * q[2])) + F(F(F(t * t) * t) * q[3]));
          const x = co(bz(px)), y = co(bz(py));
          emit(X, Y, x, y, c); X = x; Y = y;
        }
        pen = [X, Y]; break;
      }
      default: throw new Error('op ' + op);
    }
  }
  return {seg, steps, sins, raster};
}

// ------------------------------------------------------------ candidates
// Colours (RGB565). K(a, b): the constant whose flip c = K - c swaps a and b.
const RED = 0xc228, BLUE = 0x3a7a, YEL = 0xe6c4, GRN = 0x5d2b, VIO = 0xa2b5, TAN = 0x8c51, SKIN = 0xf5d3;
const RID0 = 0x9cd3, RID1 = 0x7bcf, FILL = 0x39e7, BASE = 0x4a49, STRIPE = 0x5acb, WHITE = 0xffff, DIM = 0x632c;
const K = (a, b) => a + b;

// One element (a person, a head, two strips, a dot) per cell and row: the
// JS sets the density by the cell width (12 m / per), not the plan. Side
// skeleton: cells outer, rows inner (x0 the first pillar, W px a cell, y0
// the ground, rowGap < 0 a row). Pan skeleton: the same loops over a ser()
// series of cells: 1/Z' by 3 Newton steps once a cell (pk's), x linear
// inside it, y and size from the 1/Z' at the cell's start (no shrink up
// the rows there: registers and instructions). Both give the body base
// (cell start x), W (cell width px, < 0 when the series runs right to left),
// y (row base), ms (-0.48 m in px; only the negative is kept, a register)
// and rp, a phase that steps the golden angle a cell-row (the JS anchors it
// to the world: sway).
const GOLD = 2.39996;
function side(name, h) {
  return `/** @plan ${name} inputs: x0, W, y0, rowGap, cells, sway, seed, tp */
  ${name}(rows) {
    ${h.pre || ''}
    let base = x0;
    let rp = sway;
    for (let b = 0; b < cells; b++) {
      let y = rowGap * .5 + y0;
      let ms = rowGap * .2;
      ${h.cell || ''}
      for (let k = 0; k < rows; k++) {
        ${h.row}
        y += rowGap;
        ${h.shrink ? 'ms *= .88;' : ''}
        rp += ${GOLD};
      }
      ${h.post || ''}
      base += W;
    }
  },`;
}
function pan(name, h) {
  return `/** @plan ${name} inputs: x, dx, z, dz, cells, r, sway, tp */
  ${name}(rows) {
    ${h.pre || ''}
    let px = x, pz = z, q = r;
    let base = px * q;
    let rp = sway;
    for (let b = 0; b < cells; b++) {
      const ms = q * -.48;
      let y = ms * -10 + 28;
      px += dx;
      pz += dz;
      const a1 = q * (pz * q + 2);
      const a2 = a1 * (pz * a1 + 2);
      q = a2 * (pz * a2 + 2);
      const W = px * q - base;
      ${h.cell || ''}
      for (let k = 0; k < rows; k++) {
        ${h.row}
        y += ms * 5;
        rp += ${GOLD};
      }
      ${h.post || ''}
      ${h.end || 'base += W;'}
    }
  },`;
}
// A person: a shoulder line 2s wide at Y and a head dot s above, at the
// cell's middle moved by .3 W sin(rp); clothes from a flip pair.
function person(Y = 'y', lift = '') {
  return {pre: `let dc = ${RED};`, shrink: true, row: `${lift}
        const u = (sin(rp) * .3 + .5) * W + base;
        move(u + ms, ${Y});
        line(u - ms, ${Y}, dc);
        plot(u, ${Y} + ms, ${SKIN});
        dc = ${K(RED, BLUE)} - dc;`};
}
// A head on the ridge: valley at the cell's start, a peak (1.8 +- .5 s high,
// moved by sin(rp)) and the valley at its end; under it an hl fill.
const ridge = {pre: `let rc = ${RID0};`, row: `
        const j = sin(rp);
        const vl = ms * .3 + y;
        move(base, vl);
        line((j * .15 + .5) * W + base, (j * .5 + 1.8) * ms + y, rc);
        line(base + W, vl, rc);
        rc = ${K(RID0, RID1)} - rc;`};
// B'2: the ridge, half of it a frame. A cell is a pair of slots and each
// cell-row draws one slot's head, the left on even rows and the right on odd
// ones: [bs, bs + h] with bs += 2h and h = -h a row (the right slot's head
// is drawn from its right end; its sway mirrors). Every cell starts on its
// left slot, so the slots drawn form a checkerboard; the JS moves the whole
// series one slot (4 m) on odd frames, so the drawn half swaps every frame
// (today's crowd's hand). One colour: the row flip (4 instructions) does not
// fit pan's 64 with this.
const ridgeHalf = {
  side: {cell: `
      let h = W;
      let bs = base;`, row: `
        const j = sin(rp);
        const vl = ms * .3 + y;
        move(bs, vl);
        line((j * .15 + .5) * h + bs, (j * .5 + 1.8) * ms + y, ${RID0});
        line(bs + h, vl, ${RID0});
        bs += h + h;
        h = -h;`, post: 'base += W;'},
  // pan: base itself walks (no copy, 2 instructions) and is set to the next
  // cell's start, px q, at the end (W was px q - base).
  pan: {cell: `
      let h = W * .5;`, row: `
        const j = sin(rp);
        const vl = ms * .3 + y;
        move(base, vl);
        line((j * .15 + .5) * h + base, (j * .5 + 1.8) * ms + y, ${RID0});
        line(base + h, vl, ${RID0});
        base += h + h;
        h = -h;`, end: 'base = px * q;'},
};
// Clothes: two strips a cell-row (break point moved by sin(rp)), colours
// ca (flips a row) and cb (flips a cell); a dot above the break in colour
// tp, which the JS swaps white / dim every 2 frames (all dots at once).
const strips = {pre: `let ca = ${RED}; let cb = ${YEL};`, row: `
        const u = (sin(rp) * .3 + .5) * W + base;
        move(base, y);
        line(u, y, ca);
        line(base + W, y, cb);
        plot(u, ms * 1.2 + y, tp);
        ca = ${K(RED, BLUE)} - ca;`, post: `cb = ${K(YEL, GRN)} - cb;`};
// Confetti over a base: a dot a cell-row anywhere in the row's band, its
// place from sin(rp + tp) (tp: the JS adds 2.1 rad a frame, so the dots jump
// each frame); two colours flipping a cell-row; the base is an hl fill.
const confetti = {pre: `let dc = ${RED};`, row: `
        const j = sin(rp + tp);
        plot((j * .45 + .5) * W + base, sin(j * 9.1 + rp) * ms * 2.5 + y, dc);
        dc = ${K(RED, YEL)} - dc;`};
// A wave: each cell's column of persons moved up and down by sin(.04 base +
// tp) px (base: the cell's screen x; +-1 px, 2 would not fit pan's 64), one
// SIN a cell; the JS moves tp a frame, so the bob runs along the screen like
// a stadium wave.
const wave = person();
wave.cell = `
      y += sin(base * .04 + tp);`;

// per: cells a 12 m bay; front: the front row only; fill: an hl under the
// rows from e m up (solid: one line a pixel at the nearest end, else lines
// a row: stripes; upper: the rows above the front only); tp(t): input 7 by
// frame.
const CANDS = [
  {id: 'A4', label: '人影 4/区画', h: person(), per: 4},
  {id: 'A2', label: '人影 2/区画', h: person(), per: 2},
  {id: 'A1', label: '人影 1/区画', h: person(), per: 1},
  {id: 'B', label: '稜線＋塗り', h: ridge, per: 3, fill: {from: 0, solid: true, colour: FILL}},
  {id: 'C', label: '服の帯＋明るい点', h: strips, per: 2, tp: t => (t >> 1) & 1 ? WHITE : DIM},
  {id: 'D', label: '塗り＋紙吹雪', h: confetti, per: 1, fill: {from: 0, solid: true, colour: BASE}, tp: t => t * 2.1 % (2 * PI)},
  {id: 'E', label: '人影 2＋ウェーブ', h: wave, per: 2, tp: t => t * .3 % (2 * PI)},
  {id: 'F', label: '手前 1 段だけ人影', h: person(), per: 3, front: true, fill: {from: 2.4 + .3, lines: 3, colour: STRIPE, upper: true}},
  // B' (derby-background-cost.md, "B' の変形"): B without the fill, B'1 with
  // half of it a frame (pair: a cell is two 4 m slots), B'1 with 6 m heads.
  {id: 'B1', label: "B'1 稜線（塗りなし）", h: ridge, per: 3},
  {id: 'B2', label: "B'2 B'1＋チェッカー", h: ridgeHalf, per: 3, pair: true},
  {id: 'B3', label: "B'3 稜線 2 山/12 m（塗りなし）", h: ridge, per: 2},
];
// Which candidates a sheet / GIF set shows, and the file names it writes.
const SETS = {
  look: {ids: ['A4', 'A2', 'A1', 'B', 'C', 'D', 'E', 'F'], sheet: 'derby-crowd-look-preview.png', gif: 'derby-crowd-look-'},
  bprime: {ids: ['B', 'B1', 'B2', 'B3'], sheet: 'derby-crowd-bprime-preview.png', gif: 'derby-crowd-bprime-', gifIds: ['B1', 'B2', 'B3']},
};
const planCache = new Map();
for (const c of CANDS) {
  const key = JSON.stringify(c.h);
  if (!planCache.has(key)) {
    const src = side('s' + c.id, c.h.side || c.h) + '\n' + pan('p' + c.id, c.h.pan || c.h), m = plansOf(src);
    const e = {side: m.get('s' + c.id), pan: m.get('p' + c.id), src};
    for (const k of ['side', 'pan']) {
      const cp = compilePlan(e[k]);
      e[k + 'C'] = {count: cp.count, regs: stats(cp.code).regs, warnings: cp.warnings};
    }
    planCache.set(key, e);
  }
  Object.assign(c, planCache.get(key));
}
const compiled = new Map();
const rowsOf = (plan, args) => {
  let c = compiled.get(plan);
  if (!c) compiled.set(plan, c = compilePlan(plan));
  return assemble(c.code, args);
};

// ------------------------------------------------------------ cameras
const D_NR = 11, D_FR = 22.6;
function sideScene(tier, g, t) {
  const f = 100, h = 9.7, hy = 33, k = KN[tier], x0 = g - 880 / f, q = f / 40;
  const sx = w => 120 + (w - x0) * q, gy = hy + h * q, ty = e => hy + (h - e) * q;
  const j = flo((x0 - 130 / q) / 12), a = sx(j * 12), dx = 12 * q;
  const n = Math.min(flo((700 - a) / dx), Math.ceil((250 - a) / dx) + 1);
  const crowdArgs = k.concat(1 / k[2], Math.ceil(k[2] / 2));
  const today = [{plan: TODAY.get('stands'), in: [a, dx, gy, -2.4 * q, n, 0, 0, ty(13.5)], args: k.slice(0, 4), bg: 1},
    {plan: TODAY.get('crowd'), in: [a, dx, gy, -2.4 * q, n, j * k[2] * 2.39996 % (2 * PI) - 2 * PI * rnd(n * k[2] * .191), ((t >> 3) ^ t) & 1, t & 1], args: crowdArgs}];
  const hlSide = (e0, cs, m, col) => split({plan: TODAY.get('hl'), in: [-5, q, 245, q, (hy - 28) / q + h - e0, cs, m, col], args: []});
  const cand = c => {
    const rows = c.front ? 1 : k[1], m = c.per, out = [], ph = j * m * rows * GOLD, half = n * m * rows * GOLD / 2;
    if (c.fill) {
      const r0 = c.fill.upper ? k[1] - 1 : k[1], m = c.fill.solid ? Math.ceil(r0 * 2.4 * q) + 1 : r0 * c.fill.lines;
      if (r0 > 0) out.push(...hlSide(c.fill.from, c.fill.solid ? -r0 * 2.4 / (m - 1) : -2.4 / c.fill.lines, m, c.fill.colour));
    }
    if (c.pair) {
      // Pairs of slots (cw m) on a world grid, the grid moved one slot on odd
      // frames; from one pair left of the first pillar, whole pairs up to
      // today's crowd's right end.
      const sl = 12 / m, cw = 2 * sl, sh = t & 1, ci = flo(j * 12 / cw) - 1, xs = sx(ci * cw + sh * sl);
      const cells = flo((a + n * dx - xs) / (cw * q)), p = (ci * rows + sh * .5) * GOLD;
      out.push({plan: c.side, in: [xs, sl * q, gy, -2.4 * q, cells, p % (2 * PI) - 2 * PI * rnd(cells * rows * GOLD / 4 / PI), 0, 0], args: [rows]});
      return out;
    }
    out.push({plan: c.side, in: [a, dx / m, gy, -2.4 * q, n * m, ph % (2 * PI) - 2 * PI * rnd(half / 2 / PI), 0, c.tp ? c.tp(t) : 0], args: [rows]});
    return out;
  };
  return {today, cand, window: [flo(ty(13.5) - 2 * 2.4 * q) - 2, Math.ceil(gy) + 3], v: {q, x0, gy, k}};
}
function panScene(tier, g, t) {
  const k = KN[tier], units = [100, 460, 820];
  let u = units[0];
  for (const v of units) if (Math.abs(v - g) < Math.abs(u - g)) u = v;
  const ax = g - u, az = (D_NR + D_FR) / 2 + 14, e = Math.hypot(ax, az);
  const pc = [u, -14, ax / e, az / e, Math.min(1500, Math.max(200, 14 * e / 2.4)), e];
  const zf = pc[5] + (tier ? 75 : 150);
  const S = {};
  const pj = (x, z) => { const a = x - pc[0], b = z - pc[1], Z = (a * pc[2] + b * pc[3]) / pc[4]; return [a * pc[3] - b * pc[2] + 120 * Z, Z]; };
  const lim = (c0, c1) => { if (c1 > 0) S.Lo = Math.max(S.Lo, -c0 / c1); else if (c1 < 0) S.Hi = Math.min(S.Hi, -c0 / c1); else if (c0 < 0) S.Hi = -1e9; };
  const inr = (q, U, V, e) => { const z = q[1] + e * U, x = (q[0] + e * V) / z; return z > .02 && x > -400 && x < 640; };
  // ser() of derby_scene.js; parity false: no checkerboard drop (candidates),
  // ph: the per-point phase increment the plan adds (2.39996 a column for pk,
  // a bay's for the candidates).
  function ser(plan, args, w, s, L, a, b, {parity = true, dph = 2.39996, plus = 1.8, off = 0} = {}) {
    const out = [];
    const q = S.SQ = pj(off, w), uu = S.SU = pc[2] / pc[4], v = S.SV = pc[3] + 120 * uu;
    S.Lo = -1e9; S.Hi = 1e9;
    lim(q[1] - .02, uu); lim(zf / pc[4] - q[1], -uu); lim(q[0] + 40 * q[1], v + 40 * uu); lim(280 * q[1] - q[0], 280 * uu - v);
    if (!(S.Lo < S.Hi)) return out;
    const o = uu < 0 ? -1 : 1, U = uu * o, V = v * o, t1 = o > 0 ? S.Hi : -S.Lo;
    let kk = s, Z = Math.sqrt(Math.abs(v * q[1] - q[0] * uu) * s / L), n0 = flo((o > 0 ? S.Lo : -S.Hi) / s) * s, ee;
    while (Z < q[1] + t1 * U && 2 * kk * U < .3 * (q[1] + t1 * U) && kk < 64 * s) kk *= 2, Z *= Math.SQRT2;
    ee = Math.ceil(t1 / kk) * kk;
    if (!inr(q, U, V, ee)) ee -= kk;
    if (!inr(q, U, V, n0)) n0 += s;
    if (parity && !L && (rnd(o * ee / s) + t) & 1) ee -= s;
    for (;;) {
      Z /= Math.SQRT2;
      const hh = kk > s ? Math.ceil(Math.max(U > 1e-7 ? (Math.max(Z, kk * U / .3) - q[1]) / U : -1e9, n0) / kk) * kk : n0, c = rnd((ee - hh) / kk), z = q[1] + ee * U;
      if (c > 0) out.push({plan, args, in: [q[0] + ee * V, -V * kk, -z, U * kk, Math.min(255, c), 1 / z,
        L ? a : -ee / s * dph % (2 * PI) - 2 * PI * rnd(c * dph / 4 / PI) + plus, b],
        // (crowd_prim.mjs) the series' first point and its step along the stand, m
        wx: o * ee, wk: -o * kk});
      if (kk === s) return out;
      ee = Math.min(ee, hh); kk /= 2;
    }
  }
  const hl = (c0, cs, m, col) => {
    const a = S.SQ[1] + S.Lo * S.SU, b = S.SQ[1] + S.Hi * S.SU;
    return S.Lo < S.Hi ? split({plan: TODAY.get('hl'), args: [], in: [(S.SQ[0] + S.Lo * S.SV) / a, 1 / a, (S.SQ[0] + S.Hi * S.SV) / b, 1 / b, c0, cs, m, col]}) : [];
  };
  const stands = ser(TODAY.get('prail'), [], 40, 12, 4, 31727, -7.5).concat(hl(6, -2.4, k[0], 21130));
  stands.forEach(d => { d.bg = 1; });
  const today = stands.concat(ser(TODAY.get('pk'), KN[tier].concat(1 / k[2], Math.ceil(k[2] / 2)).slice(0, 2), 40, 12 / k[2], 0, 0,
    46496 + 12650 * (((t >> 3) ^ t) & 1)));
  const cand = c => {
    const rows = c.front ? 1 : k[1], out = [];
    ser(TODAY.get('prail'), [], 40, 12, 4, 31727, -7.5); // restores the stand's view for hl
    if (c.fill) {
      const r0 = c.fill.upper ? k[1] - 1 : k[1];
      const qn = Math.max(1 / (S.SQ[1] + S.Lo * S.SU), 1 / (S.SQ[1] + S.Hi * S.SU));
      const m = c.fill.solid ? Math.min(255, Math.ceil(r0 * 2.4 * qn) + 1) : r0 * c.fill.lines;
      if (r0 > 0) out.push(...hl(6 - c.fill.from, c.fill.solid ? -r0 * 2.4 / (m - 1) : -2.4 / c.fill.lines, m, c.fill.colour));
    }
    // pair: cells of two slots, the series moved one slot (off) on odd frames.
    const cw = (c.pair ? 24 : 12) / c.per;
    out.push(...ser(c.pan, [rows], 40, cw, 0, 0, c.tp ? c.tp(t) : 0, {parity: false, dph: rows * GOLD, plus: c.pair ? (t & 1) * .5 * GOLD : 0, off: c.pair ? (t & 1) * cw / 2 : 0}));
    return out;
  };
  return {today, cand, pc, zf, pj, stands};
}
// An hl fill cut into draws under the 8,192 raster steps a draw (a line's
// cost is its longer extent + 1; the ends joined add two verticals).
function split(d) {
  d.fill = 1;
  const [x0, , x1, , c0, cs, m] = d.in, w = Math.min(Math.abs(x1 - x0), 1200) + 2;
  const parts = Math.ceil(m * w / 7000);
  if (parts <= 1) return [d];
  const out = [];
  for (let i = 0, done = 0; i < parts; i++) {
    const mi = Math.round(m * (i + 1) / parts) - done;
    out.push({...d, in: d.in.slice(0, 4).concat(c0 + cs * done, cs, mi, d.in[7])});
    done += mi;
  }
  return out;
}
// Runs a draw list: segments and counts (crowd draws only; bg: the stands).
function run(list, check = false) {
  const segs = [], cost = {draws: 0, steps: 0, sins: 0, points: 0, lines: 0, raster: 0, linePx: 0}, plan = {...cost};
  for (const d of list) {
    const r = runIR(rowsOf(d.plan, d.args), d.in);
    if (check) {
      const ref = reference(d.plan, d.in, d.args);
      if (ref.status !== 'DONE' || JSON.stringify(ref.seg) !== JSON.stringify(r.seg))
        throw new Error(`${d.plan.name}: stepper and reference differ (${ref.status}, ${ref.seg.length} vs ${r.seg.length})`);
    }
    segs.push(...r.seg);
    if (d.bg) continue;
    for (const C of d.fill ? [cost] : [cost, plan]) {
      C.draws++; C.steps += r.steps; C.sins += r.sins; C.raster += r.raster;
      for (const [x0, y0, x1, y1] of r.seg) {
        if (x0 === x1 && y0 === y1) C.points++;
        else { C.lines++; C.linePx += Math.max(Math.abs(x1 - x0), Math.abs(y1 - y0)) + 1; }
      }
    }
  }
  return {segs, cost, plan};
}
// Estimate (not measured): calibrated on today's checkerboard crowd, 1 draw
// 523 us at 767 steps / 66 dots / 66 SIN (device, derby-background-cost.md):
// 50 a draw + .45 a step + 1.0 a SIN beyond its step + 1.0 a segment emitted
// gives 527. Band drawing (outside the draw): .85 a segment (17 bands x 50 ns
// rejection) + .02 a pixel of line (a guess, not measured).
const usDraw = c => 50 * c.draws + .45 * c.steps + 1.0 * c.sins + 1.0 * (c.points + c.lines);
const usBand = c => .85 * (c.points + c.lines) + .02 * c.linePx;

// ------------------------------------------------------------ pictures
const BG = [0x18, 0x24, 0x1c];
const rgb = c => [(c >> 11 & 31) * 255 / 31 | 0, (c >> 5 & 63) * 255 / 63 | 0, (c & 31) * 255 / 31 | 0];
const img = (w, h, fill = BG) => { const d = new Uint8Array(w * h * 3); for (let i = 0; i < w * h; i++) d.set(fill, i * 3); return {w, h, d}; };
function raster(segs, y0, h) {
  const I = img(240, h);
  for (let [x0, ya, x1, yb, c] of segs) {
    const col = rgb(c);
    let dx = Math.abs(x1 - x0), dy = -Math.abs(yb - ya), sx = x0 < x1 ? 1 : -1, sy = ya < yb ? 1 : -1, err = dx + dy;
    for (;;) {
      if (x0 >= 0 && x0 < 240 && ya - y0 >= 0 && ya - y0 < h) I.d.set(col, ((ya - y0) * 240 + x0) * 3);
      if (x0 === x1 && ya === yb) break;
      const e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; ya += sy; }
    }
  }
  return I;
}
function blit(dst, src, x, y) { for (let r = 0; r < src.h; r++) dst.d.set(src.d.subarray(r * src.w * 3, (r + 1) * src.w * 3), ((y + r) * dst.w + x) * 3); }
function grid(cells, gap = 4, col = [255, 255, 255]) { // cells[row][col], rows may differ in height
  const W = cells[0].length * 240 + (cells[0].length - 1) * gap, hs = cells.map(r => r[0].h);
  const out = img(W, hs.reduce((a, b) => a + b, 0) + (cells.length - 1) * gap, col);
  let y = 0;
  cells.forEach((r, i) => { r.forEach((c, j) => blit(out, c, j * (240 + gap), y)); y += hs[i] + gap; });
  return out;
}
function scale(I, s) {
  const o = {w: I.w * s, h: I.h * s, d: new Uint8Array(I.w * s * I.h * s * 3)};
  for (let y = 0; y < o.h; y++) for (let x = 0; x < o.w; x++) o.d.set(I.d.subarray(((y / s | 0) * I.w + (x / s | 0)) * 3, ((y / s | 0) * I.w + (x / s | 0)) * 3 + 3), (y * o.w + x) * 3);
  return o;
}
function png(file, I) {
  const raw = Buffer.alloc((I.w * 3 + 1) * I.h);
  for (let y = 0; y < I.h; y++) Buffer.from(I.d.buffer, y * I.w * 3, I.w * 3).copy(raw, y * (I.w * 3 + 1) + 1);
  const chunk = (t, d) => { const b = Buffer.alloc(12 + d.length); b.writeUInt32BE(d.length, 0); b.write(t, 4); d.copy(b, 8); b.writeUInt32BE(zlib.crc32(Buffer.concat([Buffer.from(t), d])) >>> 0, 8 + d.length); return b; };
  const ih = Buffer.alloc(13); ih.writeUInt32BE(I.w, 0); ih.writeUInt32BE(I.h, 4); ih[8] = 8; ih[9] = 2;
  fs.mkdirSync(path.dirname(file), {recursive: true});
  fs.writeFileSync(file, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ih), chunk('IDAT', zlib.deflateSync(raw, {level: 9})), chunk('IEND', Buffer.alloc(0))]));
  console.log('wrote', path.relative(ROOT, file), `${I.w}x${I.h}`);
}
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
  console.log('wrote', path.relative(ROOT, file), `${w}x${h}, ${frames.length} frames, ${delayCs * 10} ms`);
}

// ------------------------------------------------------------ scenes
const SCENES = [];
for (const tier of [0, 1, 2]) for (const cam of ['side', 'pan']) for (const g of [300, 800]) SCENES.push({tier, cam, g});
const sceneName = s => `${TIERS[s.tier]} ${s.cam === 'side' ? '横見' : '首振り'} WIDE ${s.g} m`;
const build = (s, t) => (s.cam === 'side' ? sideScene : panScene)(s.tier, s.g + 16 / 30 * t, t);
function windowOf(s) {
  const sc = build(s, 0);
  if (sc.window) return sc.window;
  const {segs} = run(sc.stands);
  let lo = 135, hi = 0;
  for (const [, y0, , y1] of segs) for (const y of [y0, y1]) { lo = Math.min(lo, Math.max(0, y)); hi = Math.max(hi, Math.min(134, y)); }
  return [Math.max(0, lo - 3), Math.min(135, hi + 4)];
}
function frame(s, t, which, win, check) {
  const sc = build(s, t);
  const bg = sc.today.filter(d => d.bg), list = which === 'today' ? sc.today : bg.concat(sc.cand(which));
  const r = run(list, check);
  return {I: raster(r.segs, win[0], win[1] - win[0]), cost: r.cost, plan: r.plan};
}

const argv = process.argv.slice(2), outDir = argv.includes('--out') ? path.resolve(argv[argv.indexOf('--out') + 1]) : path.join(ROOT, 'docs/apps');
// --set look (A4..F, the first sheet) or bprime (B and its variants B'1..3).
const SET = SETS[argv.includes('--set') ? argv[argv.indexOf('--set') + 1] : 'look'];
if (!SET) { console.log('--set look|bprime'); process.exit(2); }
const SHOWN = SET.ids.map(id => CANDS.find(c => c.id === id)), COLS = ['today', ...SHOWN];
function table() {
  console.log('plans (instructions / registers, compiler): today crowd ' + compilePlan(TODAY.get('crowd')).count + ', pk ' + compilePlan(TODAY.get('pk')).count + ', hl ' + compilePlan(TODAY.get('hl')).count);
  for (const c of SHOWN) console.log(`${c.id} ${c.label}: side ${c.sideC.count} instr / ${c.sideC.regs} regs, pan ${c.panC.count} / ${c.panC.regs}${c.fill ? ' + hl (27)' : ''}${c.sideC.warnings.length ? ' warn ' + c.sideC.warnings : ''}`);
  console.log('\nscene | form | draws | steps | SIN | points | lines | line px | draw us (est) | band us (est) | x today | plan only us (fill as a Kasane rect) | x today');
  const sum = {}, sumP = {}, sumB = {};
  for (const s of SCENES) {
    const win = windowOf(s);
    let base = 0, baseB = 0;
    for (const c of COLS) {
      const id = c === 'today' ? 'today' : c.id;
      // Averaged over 8 frames (the checkerboard halves alternate).
      const acc = {draws: 0, steps: 0, sins: 0, points: 0, lines: 0, raster: 0, linePx: 0}, pa = {...acc};
      for (let t = 0; t < 8; t++) {
        const {cost, plan} = frame(s, t, c === 'today' ? 'today' : c, win, true);
        for (const k in acc) { acc[k] += cost[k] / 8; pa[k] += plan[k] / 8; }
      }
      const us = usDraw(acc), up = usDraw(pa);
      if (id === 'today') base = us;
      if (id === 'B') baseB = us;
      if (baseB) (sumB[id] ??= []).push(us / baseB);
      (sum[id] ??= []).push(us / base);
      (sumP[id] ??= []).push(up / base);
      console.log(`${sceneName(s)} | ${id} | ${acc.draws.toFixed(1)} | ${acc.steps.toFixed(0)} | ${acc.sins.toFixed(0)} | ${acc.points.toFixed(0)} | ${acc.lines.toFixed(0)} | ${acc.linePx.toFixed(0)} | ${us.toFixed(0)} | ${usBand(acc).toFixed(0)} | ${(us / base).toFixed(2)} | ${up.toFixed(0)} | ${(up / base).toFixed(2)}`);
    }
  }
  const mean = o => Object.entries(o).map(([k, v]) => `${k} ${(v.reduce((a, b) => a + b) / v.length).toFixed(2)}`).join(', ');
  console.log('\nmean ratio to today (draw us, 12 scenes): ' + mean(sum));
  console.log('the same, fill drawn by Kasane instead (plan only): ' + mean(sumP));
  if (SET === SETS.bprime) console.log('mean ratio to B (draw us, 12 scenes): ' + mean(sumB));
  console.log('stepper = plan_js.mjs float32 reference on every draw: PASS');
}
function sheet() {
  const cells = SCENES.map(s => { const win = windowOf(s); return COLS.map(c => frame(s, 0, c === 'today' ? 'today' : c, win).I); });
  png(path.join(outDir, SET.sheet), scale(grid(cells), +(process.env.SCALE || 3)));
  console.log('rows: ' + SCENES.map(sceneName).join(', '));
  console.log('columns: today, ' + SHOWN.map(c => c.id + ' ' + c.label).join(', ') + '; t = 0, x3');
}
function gifs() {
  const wins = SCENES.map(windowOf);
  for (const c of (SET.gifIds || SET.ids).map(id => CANDS.find(c => c.id === id))) {
    const frames = [];
    for (let t = 0; t < 8; t++) frames.push(scale(grid(SCENES.map((s, i) => [frame(s, t, 'today', wins[i]).I, frame(s, t, c, wins[i]).I])), 2));
    gif(path.join(outDir, `${SET.gif}${c.id}.gif`), frames, 3);
  }
  console.log('each GIF: columns today | candidate, rows as the sheet, t = 0..7 at race pace (16 m/s), x2, 30 ms a frame');
}
// The panning view's question: is one Newton a cell and x linear inside it
// enough? Over the cells in view (x -40..280, nearer than zf): the largest
// |linear - exact| in x, and in y the error of taking the cell's start 1/Z'
// for its whole width (the plans' choice), at the front row (1.2 m up, 4.8
// m under the camera: the largest lever), px.
function interp() {
  const sizes = [12, 6, 4, 3];
  console.log('scene | f | ' + sizes.map(m => `x err ${m} m | y err ${m} m`).join(' | '));
  for (const s of SCENES.filter(s => s.cam === 'pan')) {
    const sc = panScene(s.tier, s.g, 0);
    const P = w => { const p = sc.pj(w, 40); return p[1] > .02 ? [p[0] / p[1], 1 / p[1], p[1] * sc.pc[4]] : null; };
    const row = sizes.map(m => {
      let ex = 0, ey = 0;
      for (let w0 = -240; w0 < 1240; w0 += m) {
        const A = P(w0), B = P(w0 + m);
        if (!A || !B || Math.max(A[0], B[0]) < -40 || Math.min(A[0], B[0]) > 280 || A[2] > sc.zf) continue;
        for (let u = .05; u < 1; u += .05) {
          const M = P(w0 + m * u);
          ex = Math.max(ex, Math.abs(A[0] + (B[0] - A[0]) * u - M[0]));
          ey = Math.max(ey, Math.abs((A[1] - M[1]) * 4.8));
        }
      }
      return `${ex.toFixed(2)} | ${ey.toFixed(2)}`;
    });
    console.log(`${sceneName(s)} | ${sc.pc[4].toFixed(0)} | ${row.join(' | ')}`);
  }
}
export {CANDS, SCENES, TIERS, KN, TODAY, sceneName, build, windowOf, rowsOf, runIR, split, run, raster, grid, scale, png, gif, rgb, BG, ROOT};
if (!process.env.NOMAIN) switch (argv[0]) {
  case 'table': table(); break;
  case 'sheet': sheet(); break;
  case 'gif': gifs(); break;
  case 'interp': interp(); break;
  case 'src': for (const c of SHOWN) console.log(c.src); break;
  default: console.log('usage: crowd_look.mjs table|sheet|gif|interp|src [--out DIR]'); process.exit(2);
}
