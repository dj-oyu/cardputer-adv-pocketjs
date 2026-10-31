// Shiki v1 line-program reference engine (stage 1 of docs/kasane/shiki-spec.md §11).
//
// Written from the spec text alone, independently of the C engine, so that the
// two can be compared bit for bit by a random differential test (§10.1). Every
// formula below is the spec's mathematical definition evaluated exactly: 64-bit
// products go through BigInt, floor() is toward −∞ (§0), and the JS→fixed
// conversion (§2.4) is done in double exactly as written, because the C side
// does it in double too and a "better" rounding here would be a mismatch.
//
// Scope: registration (§3, argument patches, §6.1) and execution of ops 0–22
// (§4.3–§4.8; CUBIC and LINE_PATTERN as of spec v1.2). The data-region ops
// 23–26 are not implemented (stage 4), so registration rejects them, and
// rejects any header declaring regions.
//
// CLI: node tools/kasane_ir/shiki_ref.mjs CASES.txt  (one result line per case;
// the text contract is shared with the C runner, see format()).

import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';

// ---- §0 / §2 arithmetic ---------------------------------------------------

const I32_MIN = -2147483648;
const I32_MAX = 2147483647;
const B_I32_MIN = -2147483648n;
const B_I32_MAX = 2147483647n;

export const sat32 = (x) => (x < I32_MIN ? I32_MIN : x > I32_MAX ? I32_MAX : x);
export const sat16 = (x) => (x < -32768 ? -32768 : x > 32767 ? 32767 : x);
const sat32n = (x) => Number(x < B_I32_MIN ? B_I32_MIN : x > B_I32_MAX ? B_I32_MAX : x);

// Every sum/difference of two int32 is < 2^33, exact in double, so sat32 on a
// plain Number is exact. Products are not (up to 2^62), hence BigInt in MUL.
export const add32 = (a, b) => sat32(a + b);
export const sub32 = (a, b) => sat32(a - b);
export const neg32 = (a) => sat32(-a);

// §2.2 MUL: sat32(⌊a·b / 2^s⌋). BigInt >> is an arithmetic (floor) shift, which
// is exactly ⌊⌋ for negative products too.
export function mul32(a, b, s) {
  return sat32n((BigInt(a) * BigInt(b)) >> BigInt(s));
}

// §2.2 SHIFT: k ≥ 0 → sat32(a·2^k) (|a·2^31| < 2^62, exact in double);
// k < 0 → ⌊a / 2^−k⌋ (always inside int32, no saturation needed).
export function shift32(a, k) {
  if (k >= 0) return sat32(a * 2 ** k) | 0;
  return Math.floor(a / 2 ** -k) | 0;
}

// §2.3: operands are sat16'd first, then the 16-bit definition applies.
export const addH = (a, b) => sat16(sat16(a) + sat16(b));
export const subH = (a, b) => sat16(sat16(a) - sat16(b));
// |a'·b'| ≤ 2^30, exact in double; floor of division by 2^s is exact.
export const mulH = (a, b, s) => sat16(Math.floor((sat16(a) * sat16(b)) / 2 ** s)) | 0; // | 0 folds −0
export function shiftH(a, k) {
  const x = sat16(a);
  if (k >= 0) return sat16(x * 2 ** k) | 0;
  return Math.floor(x / 2 ** -k) | 0;
}

// §2.4: x = clamp(v·2^f) in double, then sign(x)·⌊|x| + 0.5⌋, also in double
// (so 0.49999999999999994 rounds to 1 exactly as C's floor(fabs(x)+0.5) does).
// Caller has already rejected non-finite v. `| 0` folds −0 to 0.
export function toFixed(v, f) {
  let x = v * 2 ** f;
  if (x < -2147483648) x = -2147483648;
  if (x > 2147483647) x = 2147483647;
  const m = Math.floor(Math.abs(x) + 0.5);
  return (x < 0 ? -m : m) | 0;
}

// ---- §4.4 sine --------------------------------------------------------------

export const SIN_TABLE = Object.freeze(
  Array.from({ length: 257 }, (_, k) => Math.round(Math.sin((k * Math.PI) / 512) * 65536)),
);

export function sinQ16(r) {
  // Low 16 bits of the two's-complement register == r mod 2^16 (non-negative).
  const p = ((r % 65536) + 65536) % 65536;
  const quad = Math.floor(p / 16384);
  let w = p % 16384;
  if (quad === 1 || quad === 3) w = 16384 - w;
  let v;
  if (w === 16384) v = SIN_TABLE[256];
  else {
    const i = Math.floor(w / 64);
    const t = w % 64;
    v = SIN_TABLE[i] + Math.floor(((SIN_TABLE[i + 1] - SIN_TABLE[i]) * t) / 64);
  }
  return (quad >= 2 ? -v : v) | 0; // | 0: no −0 out of quad 2 at w = 0
}

// ---- §4.3 opcode table ------------------------------------------------------

// Field usage per op. reg = register 0..15, inp = input index (< declared
// inputs), imm 'any' = full int32 (SET), 'int' = an int-taking imm (Q0 patch
// rule, §3), aux 'u16' = colour / shift amount. Unlisted fields must be 0.
// Ops 23–26 exist in v1 but are out of scope until stage 4 (rejected).
export const OPS = {
  SET: 0, INPUT: 1, ADD: 2, MUL: 3, SIN: 4, REPEAT: 5, END: 6, MOVE: 7, PLOT: 8,
  LINE: 9, REPEAT_REG: 10, BREAK_IF_GT: 11, PLOT_C: 12, LINE_C: 13, CUBIC: 14,
  LINE_PATTERN: 15, SUB: 16, NEG: 17, SHIFT: 18, ADD_H: 19, SUB_H: 20, MUL_H: 21,
  SHIFT_H: 22, EACH: 23, FETCH: 24, PUT: 25, LOOKUP: 26,
};

const F = (dst, a, b, imm, aux) => ({ dst, a, b, imm, aux });
const OPDEF = new Map([
  [0, F(true, false, false, 'any', false)], // SET dst, imm
  [1, F(true, 'inp', false, false, false)], // INPUT dst, a
  [2, F(true, 'reg', true, false, false)], // ADD
  [3, F(true, 'reg', true, false, 's62')], // MUL aux=s
  [4, F(true, 'reg', false, false, false)], // SIN
  [5, F(false, false, false, 'int', false)], // REPEAT imm
  [6, F(false, false, false, false, false)], // END
  [7, F(false, 'reg', true, false, false)], // MOVE a, b
  [8, F(false, 'reg', true, false, 'col')], // PLOT a, b, aux=colour
  [9, F(false, 'reg', true, false, 'col')], // LINE
  [10, F(false, 'reg', false, false, false)], // REPEAT_REG a
  [11, F(false, 'reg', true, false, false)], // BREAK_IF_GT a, b
  [12, F(true, 'reg', true, false, false)], // PLOT_C a, b, dst
  [13, F(true, 'reg', true, false, false)], // LINE_C
  [14, F('cubic', false, false, 'int', 'col')], // CUBIC dst (r[dst..dst+7]), imm=n, aux=colour (v1.2)
  [15, F('pattern', 'reg', true, 'int', 'col')], // LINE_PATTERN dst (block), a, b, imm=N, aux=colour A
  [16, F(true, 'reg', true, false, false)], // SUB
  [17, F(true, 'reg', false, false, false)], // NEG
  [18, F(true, 'reg', false, 'int', false)], // SHIFT imm=k
  [19, F(true, 'reg', true, false, false)], // ADD.H
  [20, F(true, 'reg', true, false, false)], // SUB.H
  [21, F(true, 'reg', true, false, 's31')], // MUL.H aux=s
  [22, F(true, 'reg', false, 'int', false)], // SHIFT.H imm=k
]);
// ints taken by imm per op for the Q0 patch rule — includes the out-of-scope
// ones for completeness, though those never reach the check.
const INT_IMM_OPS = new Set([5, 14, 15, 18, 22, 24, 25, 26]);

// ---- §3 assembler (shared with the differential driver) -------------------

// assemble({inputs:[q..], args:[q..], regions:[[type,q,k,flags]..],
//           patches:[{instr,arg,field,rsv?} | [instr,arg,field]..],
//           code:[{op,dst,a,b,imm,aux,rsv?}..], raw overrides...}) → Uint8Array
// `op` may be a number or an OPS name ('ADD_H' etc). Overrides (magic, version,
// kind, n, nInputs, nArgs, nRegions, nPatches, reserved, pad) let tests build
// malformed headers; they default to the consistent values.
export function assemble(spec) {
  const inputs = spec.inputs ?? [];
  const args = spec.args ?? [];
  const regions = spec.regions ?? [];
  const patches = (spec.patches ?? []).map((p) =>
    Array.isArray(p) ? { instr: p[0], arg: p[1], field: p[2], rsv: p[3] ?? 0 } : p,
  );
  const code = spec.code ?? [];
  const headLen = 12 + inputs.length + args.length + 4 * regions.length + 4 * patches.length;
  const padLen = (4 - (headLen % 4)) % 4;
  const out = new Uint8Array(headLen + padLen + 12 * code.length);
  const dv = new DataView(out.buffer);
  const magic = spec.magic ?? 'SKI1';
  for (let i = 0; i < 4; i++) out[i] = magic.charCodeAt(i) & 255;
  out[4] = spec.version ?? 1;
  out[5] = spec.kind ?? 0;
  out[6] = spec.n ?? code.length;
  out[7] = spec.nInputs ?? inputs.length;
  out[8] = spec.nArgs ?? args.length;
  out[9] = spec.nRegions ?? regions.length;
  out[10] = spec.nPatches ?? patches.length;
  out[11] = spec.reserved ?? 0;
  let o = 12;
  for (const q of inputs) out[o++] = q;
  for (const q of args) out[o++] = q;
  for (const r of regions) for (let j = 0; j < 4; j++) out[o++] = r[j] ?? 0;
  for (const p of patches) {
    out[o++] = p.instr; out[o++] = p.arg; out[o++] = p.field; out[o++] = p.rsv ?? 0;
  }
  for (let j = 0; j < padLen; j++) out[o++] = spec.pad ?? 0;
  for (const ins of code) {
    const op = typeof ins.op === 'string' ? OPS[ins.op.replace('.', '_')] : ins.op;
    out[o] = op; out[o + 1] = ins.dst ?? 0; out[o + 2] = ins.a ?? 0; out[o + 3] = ins.b ?? 0;
    dv.setInt32(o + 4, ins.imm ?? 0, true);
    dv.setUint16(o + 8, ins.aux ?? 0, true);
    dv.setUint16(o + 10, ins.rsv ?? 0, true);
    o += 12;
  }
  return out;
}

// ---- §3 / §6.1 registration -------------------------------------------------

const MAX_INSTR = 64, MAX_IO = 8, MAX_REGIONS = 4, MAX_PATCHES = 32, LOOP_DEPTH = 8;

// Returns the plan, or null on any §6.1 failure (INVALID_ARGUMENT).
export function register(code, args = []) {
  if (!(code instanceof Uint8Array)) return null;
  const len = code.length;
  if (len < 12) return null;
  const dv = new DataView(code.buffer, code.byteOffset, code.byteLength);
  if (code[0] !== 0x53 || code[1] !== 0x4b || code[2] !== 0x49 || code[3] !== 0x31) return null;
  if (code[4] !== 1 || code[5] !== 0) return null;
  const n = code[6], ni = code[7], na = code[8], nr = code[9], np = code[10];
  if (n < 1 || n > MAX_INSTR || ni > MAX_IO || na > MAX_IO || nr > MAX_REGIONS) return null;
  if (np > MAX_PATCHES || code[11] !== 0) return null;
  // Stage 1: data regions are not implemented, so any declared region is refused.
  if (nr !== 0) return null;
  const headLen = 12 + ni + na + 4 * nr + 4 * np;
  const padLen = (4 - (headLen % 4)) % 4;
  if (len !== headLen + padLen + 12 * n) return null;

  let o = 12;
  const inQ = [], argQ = [];
  for (let i = 0; i < ni; i++) { const q = code[o++]; if (q > 30) return null; inQ.push(q); }
  for (let i = 0; i < na; i++) { const q = code[o++]; if (q > 30) return null; argQ.push(q); }
  o += 4 * nr;
  const patches = [];
  let lastKey = -1;
  for (let i = 0; i < np; i++) {
    const instr = code[o], arg = code[o + 1], field = code[o + 2], rsv = code[o + 3];
    o += 4;
    if (instr >= n || arg >= na || field > 1 || rsv !== 0) return null;
    const key = instr * 2 + field; // (instr, field) strictly ascending ⇒ unique
    if (key <= lastKey) return null;
    lastKey = key;
    patches.push({ instr, arg, field });
  }
  for (let i = 0; i < padLen; i++) if (code[o++] !== 0) return null;

  // Arguments: count must match the declaration exactly; every value finite.
  if (!Array.isArray(args) || args.length !== na) return null;
  for (const v of args) if (typeof v !== 'number' || !Number.isFinite(v)) return null;
  const argFixed = args.map((v, i) => toFixed(v, argQ[i]));

  const ins = [];
  for (let i = 0; i < n; i++, o += 12) {
    if (dv.getUint16(o + 10, true) !== 0) return null; // reserved 2 bytes
    ins.push({ op: code[o], dst: code[o + 1], a: code[o + 2], b: code[o + 3],
      imm: dv.getInt32(o + 4, true), aux: dv.getUint16(o + 8, true) });
  }

  // Apply patches, then validate the patched program (§6.1: "after" patching).
  for (const p of patches) {
    const I = ins[p.instr];
    const v = argFixed[p.arg];
    if (p.field === 1) {
      if (argQ[p.arg] !== 0) return null; // aux patches are Q0
      if (v < 0 || v > 65535) return null; // must be representable in the u16 field
      I.aux = v;
    } else {
      if (INT_IMM_OPS.has(I.op) && argQ[p.arg] !== 0) return null;
      I.imm = v;
    }
  }

  const matchEnd = new Int32Array(n).fill(-1);
  const stack = [];
  for (let i = 0; i < n; i++) {
    const I = ins[i];
    const d = OPDEF.get(I.op);
    if (!d) return null; // reserved, or out of scope until stage 4 (23–26)
    // §6.1: a block must fit the 16 registers (CUBIC 8 → dst 0..8, LINE_PATTERN 6 → 0..10).
    const dstMax = d.dst === 'cubic' ? 8 : d.dst === 'pattern' ? 10 : 15;
    if (d.dst ? I.dst > dstMax : I.dst !== 0) return null;
    if (d.a === 'reg') { if (I.a > 15) return null; }
    else if (d.a === 'inp') { if (I.a >= ni) return null; }
    else if (I.a !== 0) return null;
    if (d.b ? I.b > 15 : I.b !== 0) return null;
    if (d.aux === 's62') { if (I.aux > 62) return null; }
    else if (d.aux === 's31') { if (I.aux > 31) return null; }
    else if (d.aux === false && I.aux !== 0) return null;
    if (d.imm === false && I.imm !== 0) return null;
    switch (I.op) {
      case 5: case 14: if (I.imm < 1 || I.imm > 255) return null; break;
      case 15: if (I.imm < 1 || I.imm > 24) return null; break;
      case 18: if (I.imm < -31 || I.imm > 31) return null; break;
      case 22: if (I.imm < -15 || I.imm > 14) return null; break;
    }
    if (I.op === 5 || I.op === 10) {
      if (stack.length >= LOOP_DEPTH) return null;
      stack.push(i);
    } else if (I.op === 6) {
      if (stack.length === 0) return null;
      matchEnd[stack.pop()] = i;
    } else if (I.op === 11) {
      if (stack.length === 0) return null;
    }
  }
  if (stack.length !== 0) return null;
  return Object.freeze({ n, inQ, ins, matchEnd });
}

// ---- §4 execution -----------------------------------------------------------

const MAX_STEPS = 10000, MAX_SEGS = 1024, MAX_RASTER = 8192;

const px = (raw) => Math.floor((raw + 32768) / 65536); // §4.5 (v1.1): nearest, halves up; exact in double
const inRange = (p) => p >= -480 && p <= 720;
// §4.7: R(p, q) = the integer nearest p / q, halves toward +∞, ⌊(2p + q) / 2q⌋
// with both signs flipped for a negative q; M(v, m) = v − m·⌊v / m⌋. BigInt in,
// Number out (every result here fits an int32).
const floorDiv = (a, b) => { const q = a / b; return a % b !== 0n && (a < 0n) !== (b < 0n) ? q - 1n : q; };
const R = (p, q) => { if (q < 0n) { p = -p; q = -q; } return Number(floorDiv(2n * p + q, 2n * q)); };
const M = (v, m) => v - m * Math.floor(v / m);
const bitLength = (v) => v.toString(2).length; // v ≥ 1
const i16 = (v) => (v << 16) >> 16;            // the frame entry's int16 field

// ---- §4.6 / §4.7 / §4.8.4 drawing, on a context {segs, pen, raster} --------
// Exported so test_shiki_compile.mjs can draw shiki_fix's values the same way.

// §4.8.4 for one plain segment; the status to stop with, or null.
export function addSeg(ctx, seg) {
  const cost = Math.max(Math.abs(seg[2] - seg[0]), Math.abs(seg[3] - seg[1])) + 1;
  if (ctx.segs.length >= MAX_SEGS || ctx.raster + cost > MAX_RASTER) return 'LIMIT';
  ctx.raster += cost;
  ctx.segs.push(seg);
  return null;
}

// §4.6 CUBIC (v1.2): P = the 8 raw Q16 control coordinates. t in Q16, the
// weights and the sum by §2.2's MUL (s = 16) and ADD, every point
// range-checked; the pen moves only when every segment is added.
export function cubicOp(ctx, P, n, colour) {
  for (const r of P) if (!inRange(px(r))) return 'INVALID';
  let prevX = px(P[0]), prevY = px(P[1]);
  for (let k = 1; k <= n; k++) {
    const t = Math.floor((k * 65536) / n), u = 65536 - t;
    const uu = mul32(u, u, 16), tt = mul32(t, t, 16);
    const w = [mul32(uu, u, 16), 3 * mul32(uu, t, 16), 3 * mul32(u, tt, 16), mul32(tt, t, 16)];
    const coord = (o) => {
      let acc = mul32(w[0], P[o], 16);
      for (let i = 1; i < 4; i++) acc = add32(acc, mul32(w[i], P[2 * i + o], 16));
      return px(acc);
    };
    const x = coord(0), y = coord(1);
    if (!inRange(x) || !inRange(y)) return 'INVALID';
    const e = addSeg(ctx, [prevX, prevY, x, y, colour]);
    if (e) return e;
    prevX = x; prevY = y;
  }
  ctx.pen.valid = true; ctx.pen.x = prevX; ctx.pen.y = prevY;
  return null;
}

// §4.7 LINE_PATTERN (v1.2): xr, yr = the end's raw Q16 coordinates, block =
// the six block registers, N = the period, colourA = aux.
export function patternOp(ctx, xr, yr, block, N, colourA) {
  const { pen } = ctx;
  const x = px(xr);
  if (!inRange(x)) return 'INVALID';
  const y = px(yr);
  if (!inRange(y)) return 'INVALID';
  if (!pen.valid) { pen.valid = true; pen.x = x; pen.y = y; return null; }
  const [bits, colB, u0, u1, w0, w1] = block;
  if (bits < 0 || bits > 0xffffff) return 'INVALID';
  if (colB < -1 || colB > 65535) return 'INVALID';
  if (u0 < -(2 ** 29) || u0 > 2 ** 29 || u1 < -(2 ** 29) || u1 > 2 ** 29) return 'INVALID';
  if (w0 < -(2 ** 24) || w0 > 2 ** 24 || w1 < -(2 ** 24) || w1 > 2 ** 24) return 'INVALID';
  let c = 1;
  if ((w0 > 0 && w1 > 0) || (w0 < 0 && w1 < 0)) {
    const W = Math.max(Math.abs(w0), Math.abs(w1)), V = Math.min(Math.abs(w0), Math.abs(w1));
    c = 50 * W < 51 * V ? 1 : 5 * W < 6 * V ? 2 : 5 * W < 8 * V ? 4 : 8;
  }
  const pX = pen.x, pY = pen.y, wrap = 256 * N, hasB = colB >= 0 ? 1 : 0;
  let ax = pX, ay = pY, ua = u0;
  for (let j = 1; j <= c; j++) {
    let bx = x, by = y, ub = u1;
    if (j < c) {
      bx = pX + R(BigInt((x - pX) * j), BigInt(c));
      by = pY + R(BigInt((y - pY) * j), BigInt(c));
      const A = Math.abs(w0), B = Math.abs(w1);
      const e = Math.max(0, bitLength(Math.max(A, B)) - 13);
      const A2 = Math.floor(A / 2 ** e), B2 = Math.floor(B / 2 ** e);
      const lam = R(BigInt(B2 * j) * 16384n, BigInt(A2 * (c - j) + B2 * j));
      ub = u0 + R(BigInt(u1 - u0) * BigInt(lam), 16384n);
    }
    const s = Math.max(Math.abs(bx - ax), Math.abs(by - ay));
    const u0q = M(R(BigInt(ua), 256n), wrap);
    let duq = 0;
    if (s !== 0) {
      const d = R(BigInt(ub - ua), BigInt(256 * s));
      if (Math.abs(d) >= 65536) return 'INVALID';
      duq = M(d, wrap);
    }
    if (ctx.segs.length >= MAX_SEGS - 1 || ctx.raster + s + 1 > MAX_RASTER) return 'LIMIT';
    ctx.raster += s + 1;
    ctx.segs.push([ax + 4096, ay, bx, by, colourA]);
    ctx.segs.push([i16(bits & 0xffff), i16(0x8000 | (bits >> 16) | (N << 8) | (hasB << 13)), i16(u0q),
      i16(0x8000 | duq), hasB ? colB : 0]);
    ax = bx; ay = by; ua = ub;
  }
  pen.valid = true; pen.x = x; pen.y = y;
  return null;
}

export function draw(plan, inputs = []) {
  const regs = new Int32Array(16);
  const pen = { valid: false, x: 0, y: 0 };
  const segs = [];
  // §4.8.7: convert only the declared inputs; extras are discarded unchecked.
  const inp = new Array(plan.inQ.length).fill(0);
  for (let i = 0; i < plan.inQ.length && i < inputs.length; i++) {
    const v = inputs[i];
    if (typeof v !== 'number' || !Number.isFinite(v)) {
      return { status: 'BADINPUT', steps: 0, pen, regs, segs };
    }
    inp[i] = toFixed(v, plan.inQ[i]);
  }

  const { ins, matchEnd, n } = plan;
  const loops = []; // {start, left}
  let steps = 0, pc = 0;
  const end = (status) => ({ status, steps, pen, regs, segs });
  const ctx = { segs, pen, raster: 0 };

  // §4.8.3/4: colour → a → b → segment → pen; a failure stops before the pen.
  // `seg` false = MOVE (no segment); for LINE with an invalid pen it is a move.
  const plotLine = (I, colour, kind) => {
    const x = px(regs[I.a]);
    if (!inRange(x)) return 'INVALID';
    const y = px(regs[I.b]);
    if (!inRange(y)) return 'INVALID';
    let seg = null;
    if (kind === 'plot') seg = [x, y, x, y, colour];
    else if (kind === 'line' && pen.valid) seg = [pen.x, pen.y, x, y, colour];
    if (seg) {
      const cost = Math.max(Math.abs(seg[2] - seg[0]), Math.abs(seg[3] - seg[1])) + 1;
      const e = addSeg(ctx, seg);
      if (e) return e;
    }
    pen.valid = true; pen.x = x; pen.y = y;
    return null;
  };

  for (;;) {
    if (steps === MAX_STEPS) return end('LIMIT');
    steps++;
    const I = ins[pc];
    let next = pc + 1;
    let err = null;
    switch (I.op) {
      case 0: regs[I.dst] = I.imm; break;
      case 1: regs[I.dst] = inp[I.a]; break;
      case 2: regs[I.dst] = add32(regs[I.a], regs[I.b]); break;
      case 3: regs[I.dst] = mul32(regs[I.a], regs[I.b], I.aux); break;
      case 4: regs[I.dst] = sinQ16(regs[I.a]); break;
      case 5:
      case 10: {
        let count;
        if (I.op === 5) count = I.imm;
        else {
          count = regs[I.a];
          if (count < 0 || count > 255) { err = 'INVALID'; break; }
        }
        if (count === 0) next = matchEnd[pc] + 1;
        else loops.push({ start: pc, left: count });
        break;
      }
      case 6: {
        const top = loops[loops.length - 1];
        top.left--;
        if (top.left !== 0) next = top.start + 1;
        else loops.pop();
        break;
      }
      case 7: err = plotLine(I, 0, 'move'); break;
      case 8: err = plotLine(I, I.aux, 'plot'); break;
      case 9: err = plotLine(I, I.aux, 'line'); break;
      case 11:
        if (regs[I.a] > regs[I.b]) {
          const top = loops.pop();
          next = matchEnd[top.start] + 1;
        }
        break;
      case 12:
      case 13: {
        const c = regs[I.dst];
        if (c < 0 || c > 65535) { err = 'INVALID'; break; }
        err = plotLine(I, c, I.op === 12 ? 'plot' : 'line');
        break;
      }
      case 14: err = cubicOp(ctx, Array.from({ length: 8 }, (_, k) => regs[I.dst + k]), I.imm, I.aux); break;
      case 15: err = patternOp(ctx, regs[I.a], regs[I.b], Array.from({ length: 6 }, (_, k) => regs[I.dst + k]), I.imm, I.aux); break;
      case 16: regs[I.dst] = sub32(regs[I.a], regs[I.b]); break;
      case 17: regs[I.dst] = neg32(regs[I.a]); break;
      case 18: regs[I.dst] = shift32(regs[I.a], I.imm); break;
      case 19: regs[I.dst] = addH(regs[I.a], regs[I.b]); break;
      case 20: regs[I.dst] = subH(regs[I.a], regs[I.b]); break;
      case 21: regs[I.dst] = mulH(regs[I.a], regs[I.b], I.aux); break;
      case 22: regs[I.dst] = shiftH(regs[I.a], I.imm); break;
      default: throw new Error(`unreachable op ${I.op}`);
    }
    if (err) return end(err);
    pc = next;
    if (pc === n) return end('DONE');
  }
}

// ---- shared text contract ---------------------------------------------------

export function format(result) {
  if (result === null) return 'REJECT';
  const p = result.pen;
  const parts = ['RUN', result.status, result.steps, p.valid ? 1 : 0, p.valid ? p.x : 0,
    p.valid ? p.y : 0, 'R', ...Array.from(result.regs, (r) => r | 0), 'S', result.segs.length];
  for (const s of result.segs) parts.push(...s);
  return parts.join(' ');
}

// Parses one `CASE <hex> ARGS <n> ... INPUTS <m> ...` line.
export function parseCase(line) {
  const t = line.trim().split(/\s+/);
  if (t[0] !== 'CASE' || t[2] !== 'ARGS') throw new Error(`bad case line: ${line}`);
  const hex = t[1];
  if (hex.length % 2) throw new Error(`odd hex length: ${line}`);
  const code = new Uint8Array(hex.length / 2);
  for (let i = 0; i < code.length; i++) code[i] = parseInt(hex.substr(2 * i, 2), 16);
  let k = 3;
  const na = Number(t[k++]);
  const args = t.slice(k, k + na).map(Number);
  k += na;
  if (t[k++] !== 'INPUTS') throw new Error(`missing INPUTS: ${line}`);
  const ni = Number(t[k++]);
  const inputs = t.slice(k, k + ni).map(Number);
  return { code, args, inputs };
}

export function runCase(line) {
  const { code, args, inputs } = parseCase(line);
  const plan = register(code, args);
  return plan === null ? 'REJECT' : format(draw(plan, inputs));
}

export const toHex = (bytes) => Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const file = process.argv[2];
  if (!file) {
    console.error('usage: node tools/kasane_ir/shiki_ref.mjs CASES.txt');
    process.exit(2);
  }
  const out = [];
  for (const line of readFileSync(file, 'utf8').split(/\r?\n/)) {
    if (line.trim() === '' || !line.trimStart().startsWith('CASE')) continue;
    out.push(runCase(line));
  }
  process.stdout.write(out.join('\n') + (out.length ? '\n' : ''));
}
