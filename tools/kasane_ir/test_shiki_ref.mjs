// Hand-built edge cases for the Shiki v1 reference engine (shiki_ref.mjs).
// Each expectation is derived from docs/kasane/shiki-spec.md by hand, not by
// running the engine, so a failure here is a disagreement with the spec text.
// Run: node tools/kasane_ir/test_shiki_ref.mjs

import assert from 'node:assert/strict';
import {
  assemble, register, draw, format, runCase, toHex, SIN_TABLE, sinQ16, toFixed,
  mul32, shift32, mulH, shiftH, addH, subH, OPS, makeRegion, parseCase,
} from './shiki_ref.mjs';

let passed = 0, failed = 0;
function test(name, fn) {
  try { fn(); passed++; } catch (e) { failed++; console.error(`FAIL ${name}\n  ${e.message}`); }
}

const MIN = -2147483648, MAX = 2147483647;
const prog = (code, extra = {}) => assemble({ code, ...extra });
const run = (code, extra = {}, args = [], inputs = []) => {
  const plan = register(prog(code, extra), args);
  assert.ok(plan, 'expected registration to succeed');
  return draw(plan, inputs);
};
const rejects = (code, extra = {}, args = []) =>
  assert.equal(register(prog(code, extra), args), null, 'expected REJECT');
const SET = (dst, imm) => ({ op: 'SET', dst, imm });

// ---- arithmetic -------------------------------------------------------------

test('MUL floor of exact 64-bit product', () => {
  assert.equal(mul32(-3, 1, 1), -2); // ⌊−1.5⌋
  assert.equal(mul32(3, 1, 1), 1);
  assert.equal(mul32(-1, 1, 62), -1); // ⌊−2^-62⌋ = −1
  assert.equal(mul32(1, 1, 62), 0);
  assert.equal(mul32(MIN, MIN, 62), 1); // 2^62 / 2^62
  assert.equal(mul32(MIN, MAX, 62), -1); // ⌊−(2^62−2^31)/2^62⌋ = −1
  assert.equal(mul32(65536 * 3, 65536 * 5, 16), 15 * 65536);
  assert.equal(mul32(-65537, 65537, 16), -65539); // −4295098369/65536 = −65538.00..., floor −65539
  assert.equal(mul32(MAX, MAX, 0), MAX); // saturate up
  assert.equal(mul32(MIN, MAX, 0), MIN); // saturate down
  assert.equal(mul32(MIN, MIN, 0), MAX);
  // 683,565,276 · 2^16 / 2^32 (TURN, §8.3 rule 8): 1 rad → 10430.37... → 10430
  assert.equal(mul32(683565276, 65536, 32), 10430);
  assert.equal(mul32(683565276, -65536, 32), -10431);
});

test('SHIFT ±31 and saturation', () => {
  assert.equal(shift32(1, 31), MAX);
  assert.equal(shift32(-1, 31), MIN);
  assert.equal(shift32(-2, 31), MIN);
  assert.equal(shift32(0, 31), 0);
  assert.equal(shift32(MIN, -31), -1);
  assert.equal(shift32(MAX, -31), 0);
  assert.equal(shift32(-1, -31), -1);
  assert.equal(shift32(-3, -1), -2);
  assert.equal(shift32(3, -1), 1);
  assert.equal(shift32(0x10000, 15), MAX);
  assert.equal(shift32(0x10000, 14), 0x40000000);
});

test('.H operand saturation and ops', () => {
  assert.equal(addH(100000, 0), 32767); // sat16 of operand
  assert.equal(addH(-100000, 1), -32767); // sat16(-100000) = −32768, +1
  assert.equal(addH(32767, 1), 32767);
  assert.equal(subH(-32768, 1), -32768);
  assert.equal(subH(0, -100000), 32767);
  assert.equal(mulH(100000, 100000, 0), 32767);
  assert.equal(mulH(100000, 100000, 31), 0); // 32767² < 2^31 → 0
  assert.equal(mulH(-100000, 100000, 31), -1); // −32768·32767 / 2^31 → floor −1
  assert.equal(mulH(-32768, -32768, 31), 0); // 2^30/2^31 = 0.5 → 0
  assert.equal(mulH(-32768, -32768, 15), 32767); // 32768 → sat
  assert.equal(mulH(-3, 1, 1), -2);
  assert.equal(mulH(-32768, 32767, 15), -32767);
  assert.equal(shiftH(70000, 0), 32767);
  assert.equal(shiftH(1, 14), 16384);
  assert.equal(shiftH(2, 14), 32767);
  assert.equal(shiftH(-3, 14), -32768);
  assert.equal(shiftH(-70000, -15), -1);
  assert.equal(shiftH(70000, -15), 0);
});

test('§2.4 conversion', () => {
  assert.equal(toFixed(0.5, 0), 1);
  assert.equal(toFixed(-0.5, 0), -1);
  assert.equal(toFixed(-1.5, 0), -2);
  assert.equal(toFixed(2.5, 0), 3);
  assert.equal(toFixed(0.49999999999999994, 0), 1); // double add rounds up, as specified
  assert.equal(toFixed(-0, 16), 0);
  assert.ok(Object.is(toFixed(-0, 16), 0));
  assert.equal(toFixed(1e300, 0), MAX);
  assert.equal(toFixed(-1e300, 30), MIN);
  assert.equal(toFixed(32768, 16), MAX); // 2^31 clamps to 2^31−1
  assert.equal(toFixed(-32768, 16), MIN);
  assert.equal(toFixed(1.5e-7, 30), 161); // 161.06...
  assert.equal(toFixed(1 / 65536 / 2, 16), 1);
});

// ---- SIN ----------------------------------------------------------------------

test('SIN table', () => {
  assert.equal(SIN_TABLE.length, 257);
  assert.equal(SIN_TABLE[0], 0);
  assert.equal(SIN_TABLE[256], 65536);
  assert.equal(SIN_TABLE[128], 46341); // sin(π/4)·65536 = 46340.95
});

test('SIN at quadrant edges and negatives', () => {
  assert.equal(sinQ16(0), 0);
  assert.equal(sinQ16(0x4000), 65536);
  assert.equal(sinQ16(0x8000), 0); // quad 2, w 0 → −0 → 0
  assert.ok(Object.is(sinQ16(0x8000) | 0, 0));
  assert.equal(sinQ16(0xC000), -65536);
  // 0xFFFF: quad 3, w = 16383 → 16384−16383 = 1; i 0, t 1: ⌊(T1−T0)/64⌋, negated
  assert.equal(sinQ16(0xFFFF), -Math.floor(SIN_TABLE[1] / 64));
  assert.equal(sinQ16(0x10000), 0); // only low 16 bits
  assert.equal(sinQ16(-1), sinQ16(0xFFFF));
  assert.equal(sinQ16(-0x4000), -65536);
  assert.equal(sinQ16(MIN), 0);
  // p = 0x2000 → w = 8192, i = 128, t = 0
  assert.equal(sinQ16(0x2000), SIN_TABLE[128]);
  // interpolation: p = 0x20 → i 0, t 32
  assert.equal(sinQ16(0x20), Math.floor(SIN_TABLE[1] * 32 / 64));
  // quad 1 mirror: p = 0x4001 → w = 16383 → same as p = 16383
  assert.equal(sinQ16(0x4001), sinQ16(16383));
  // via the engine with a negative register
  const r = run([SET(0, -0x4000), { op: 'SIN', dst: 1, a: 0 }]);
  assert.equal(r.regs[1], -65536);
});

// ---- coordinates / drawing ------------------------------------------------------

test('coordinate rounding (v1.1: nearest, halves up) and range edges', () => {
  // raw -1 is -1/65536 px: nearest 0. -32768 (-0.5) rounds up to 0, -32769 down to -1.
  const r = run([SET(0, -1), SET(1, -480 * 65536), { op: 'PLOT', a: 0, b: 1, aux: 7 },
    SET(2, -32768), SET(3, -32769), { op: 'PLOT', a: 2, b: 3, aux: 8 }]);
  assert.equal(r.status, 'DONE');
  assert.deepEqual(r.segs, [[0, -480, 0, -480, 7], [0, -1, 0, -1, 8]]);
  assert.deepEqual([r.pen.valid, r.pen.x, r.pen.y], [true, 0, -1]);
  // -480 - 0.5 px rounds to -480; one step lower is -481: INVALID.
  const r1 = run([SET(0, -480 * 65536 - 32768), { op: 'MOVE', a: 0, b: 0 }]);
  assert.deepEqual([r1.status, r1.pen.x], ['DONE', -480]);
  const r2 = run([SET(0, -480 * 65536 - 32769), SET(1, 0), { op: 'PLOT', a: 0, b: 1 }]);
  assert.equal(r2.status, 'INVALID');
  assert.equal(r2.steps, 3);
  assert.equal(r2.pen.valid, false);
  // 720 + 0.5 - 1/65536 px is 720; 720.5 rounds up to 721: INVALID.
  const r3 = run([SET(0, 720 * 65536 + 32767), { op: 'MOVE', a: 0, b: 0 }]);
  assert.deepEqual([r3.status, r3.pen.x, r3.pen.y], ['DONE', 720, 720]);
  const r4 = run([SET(0, 720 * 65536 + 32768), SET(1, 0), { op: 'MOVE', a: 1, b: 0 }]);
  assert.equal(r4.status, 'INVALID'); // b fails after a passed
});

test('LINE: move when pen invalid, segment otherwise, failed op keeps pen', () => {
  const r = run([
    SET(0, 10 << 16), SET(1, 20 << 16),
    { op: 'LINE', a: 0, b: 1, aux: 1 }, // move only
    SET(2, 13 << 16), SET(3, 16 << 16),
    { op: 'LINE', a: 2, b: 3, aux: 2 }, // cost max(3,4)+1 = 5
    SET(4, 1000 << 16),
    { op: 'LINE', a: 4, b: 4, aux: 3 }, // INVALID, pen stays (13,16)
  ]);
  assert.equal(r.status, 'INVALID');
  assert.deepEqual(r.segs, [[10, 20, 13, 16, 2]]);
  assert.deepEqual([r.pen.x, r.pen.y], [13, 16]);
  assert.equal(r.steps, 8);
});

test('PLOT_C / LINE_C colour checked first', () => {
  const r = run([SET(0, 1000 << 16), SET(5, 65536), { op: 'PLOT_C', a: 0, b: 0, dst: 5 }]);
  assert.equal(r.status, 'INVALID');
  const r2 = run([SET(0, 3 << 16), SET(5, 65535), { op: 'PLOT_C', a: 0, b: 0, dst: 5 },
    SET(5, -1), { op: 'LINE_C', a: 0, b: 0, dst: 5 }]);
  assert.equal(r2.status, 'INVALID');
  assert.deepEqual(r2.segs, [[3, 3, 3, 3, 65535]]);
  const r3 = run([SET(0, 3 << 16), SET(1, 9 << 16), SET(5, 0), { op: 'MOVE', a: 0, b: 0 },
    { op: 'LINE_C', a: 1, b: 0, dst: 5 }]);
  assert.deepEqual(r3.segs, [[3, 3, 9, 3, 0]]);
});

// ---- limits -------------------------------------------------------------------

test('steps: DONE at exactly 10,000, LIMIT at 10,001', () => {
  // REPEAT 99 { REPEAT 49 { SET } END } END: 1 + 99·(1 + 49·2 + 1) = 9,901.
  // REPEAT 49 { SET } END: 1 + 49·2 = 99. Total 10,000 in 8 instructions.
  const code = [{ op: 'REPEAT', imm: 99 }, { op: 'REPEAT', imm: 49 }, SET(0, 1), { op: 'END' },
    { op: 'END' }, { op: 'REPEAT', imm: 49 }, { op: 'ADD', dst: 1, a: 1, b: 0 }, { op: 'END' }];
  const r = run(code);
  assert.deepEqual([r.status, r.steps, r.regs[1]], ['DONE', 10000, 49]);
  // One more instruction: the check before it sees 10,000 and does not run it.
  const r2 = run([...code, SET(4, 5)]);
  assert.deepEqual([r2.status, r2.steps, r2.regs[4], r2.regs[1]], ['LIMIT', 10000, 0, 49]);
});

test('segments: 1,024 ok, 1,025th LIMIT', () => {
  // 8·128 PLOTs = 1,024 segments, raster 1,024.
  const code = [SET(0, 0), { op: 'REPEAT', imm: 8 }, { op: 'REPEAT', imm: 128 },
    { op: 'PLOT', a: 0, b: 0, aux: 9 }, { op: 'END' }, { op: 'END' }];
  const r = run(code);
  assert.equal(r.status, 'DONE');
  assert.equal(r.segs.length, 1024);
  const r2 = run([...code, { op: 'PLOT', a: 0, b: 0 }]);
  assert.equal(r2.status, 'LIMIT');
  assert.equal(r2.segs.length, 1024);
  // MOVE is not a segment and is allowed after the limit
  const r3 = run([...code, { op: 'MOVE', a: 0, b: 0 }]);
  assert.equal(r3.status, 'DONE');
  // LINE with an invalid pen is a move too — but here the pen is valid
  const r4 = run([...code, { op: 'LINE', a: 0, b: 0 }]);
  assert.equal(r4.status, 'LIMIT');
});

test('raster: 8,192 ok, one more LIMIT', () => {
  // line from (−480,0) to (720,0): cost 1,201. 6 lines = 7,206; then 986 more.
  const x0 = -480 << 16, x1 = 720 << 16;
  const code = [SET(0, x0), SET(1, x1), SET(2, 0), { op: 'MOVE', a: 0, b: 2 },
    { op: 'REPEAT', imm: 3 }, { op: 'LINE', a: 1, b: 2 }, { op: 'LINE', a: 0, b: 2 }, { op: 'END' },
    SET(3, (-480 + 985) << 16), { op: 'LINE', a: 3, b: 2 }]; // cost 986 → 8,192
  const r = run(code);
  assert.equal(r.status, 'DONE');
  assert.equal(r.segs.length, 7);
  const r2 = run([...code, { op: 'PLOT', a: 3, b: 2 }]);
  assert.equal(r2.status, 'LIMIT');
  assert.deepEqual([r2.pen.x, r2.pen.y], [505, 0]);
});

// ---- loops --------------------------------------------------------------------

test('REPEAT_REG 0 skips, 255 runs, 256 and −1 INVALID', () => {
  const body = (n) => [SET(0, n), { op: 'REPEAT_REG', a: 0 }, { op: 'ADD', dst: 1, a: 1, b: 2 },
    { op: 'END' }, SET(3, 7)];
  const r = run([SET(2, 1), ...body(0)]);
  assert.deepEqual([r.status, r.regs[1], r.regs[3], r.steps], ['DONE', 0, 7, 4]);
  const r2 = run([SET(2, 1), ...body(255)]);
  assert.deepEqual([r2.status, r2.regs[1], r2.steps], ['DONE', 255, 3 + 510 + 1]);
  const r3 = run([SET(2, 1), ...body(256)]);
  assert.deepEqual([r3.status, r3.steps, r3.regs[3]], ['INVALID', 3, 0]);
  const r4 = run([SET(2, 1), ...body(-1)]);
  assert.equal(r4.status, 'INVALID');
  // REPEAT_REG 0 as the last loop: skipping to n ends with DONE
  const r5 = run([{ op: 'REPEAT_REG', a: 0 }, { op: 'END' }]);
  assert.deepEqual([r5.status, r5.steps], ['DONE', 1]);
});

test('BREAK_IF_GT inside nested loops leaves only the innermost', () => {
  // outer 3 × inner 10; inner breaks when r1 > r2 (r1 counts per inner iter, reset per outer)
  const r = run([
    SET(2, 4), SET(3, 1),
    { op: 'REPEAT', imm: 3 },
    SET(1, 0),
    { op: 'REPEAT', imm: 10 },
    { op: 'ADD', dst: 1, a: 1, b: 3 },
    { op: 'BREAK_IF_GT', a: 1, b: 2 },
    { op: 'ADD', dst: 4, a: 4, b: 3 },
    { op: 'END' },
    { op: 'ADD', dst: 5, a: 5, b: 3 },
    { op: 'END' },
  ]);
  assert.equal(r.status, 'DONE');
  assert.equal(r.regs[4], 12); // 4 completed bodies per outer
  assert.equal(r.regs[5], 3);
  // steps: 2 + 1 + 3·(SET 1 + REPEAT 1 + 4·(ADD,BRK,ADD,END)=16 + ADD,BRK =2 + ADD 1 + END 1) = 3 + 3·22
  assert.equal(r.steps, 3 + 3 * 22);
  // breaking out of the last loop of the program ends with DONE
  const r2 = run([SET(0, 1), { op: 'REPEAT', imm: 5 }, { op: 'BREAK_IF_GT', a: 0, b: 1 }, { op: 'END' }]);
  assert.deepEqual([r2.status, r2.steps], ['DONE', 3]);
  // not taken when equal
  const r3 = run([{ op: 'REPEAT', imm: 2 }, { op: 'BREAK_IF_GT', a: 0, b: 1 }, { op: 'END' }]);
  assert.deepEqual([r3.status, r3.steps], ['DONE', 5]);
});

test('dst may alias operands (§4.8.2)', () => {
  const r = run([SET(0, 3), { op: 'MUL', dst: 0, a: 0, b: 0, aux: 0 }, { op: 'NEG', dst: 0, a: 0 },
    SET(1, MIN), { op: 'NEG', dst: 1, a: 1 }, { op: 'SUB', dst: 2, a: 2, b: 1 }]);
  assert.deepEqual([r.regs[0], r.regs[1], r.regs[2]], [-9, MAX, -MAX]);
});

// ---- inputs -------------------------------------------------------------------

test('inputs: Q conversion, missing → 0, extras discarded, non-finite → BADINPUT', () => {
  const code = [{ op: 'INPUT', dst: 0, a: 0 }, { op: 'INPUT', dst: 1, a: 1 }];
  const plan = register(prog(code, { inputs: [16, 0] }), []);
  const r = draw(plan, [1.5, -2.5, NaN]);
  assert.deepEqual([r.status, r.regs[0], r.regs[1]], ['DONE', 98304, -3]);
  const r2 = draw(plan, [0.25]);
  assert.deepEqual([r2.regs[0], r2.regs[1]], [16384, 0]);
  const r3 = draw(plan, [1, Infinity]);
  assert.equal(format(r3), 'RUN BADINPUT 0 0 0 0 R 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 S 0');
});

// ---- registration: patches ----------------------------------------------------

test('patch into SET imm with any Q, and aux colour', () => {
  const code = [SET(0, 0), SET(1, 0), { op: 'PLOT', a: 1, b: 1, aux: 0 }];
  const ex = { args: [16, 0], patches: [[0, 0, 0], [2, 1, 1]] };
  const plan = register(prog(code, ex), [-2.5, 65535]);
  const r = draw(plan, []);
  assert.equal(r.regs[0], -163840);
  assert.deepEqual(r.segs, [[0, 0, 0, 0, 65535]]);
  // aux colour out of u16 → reject; non-Q0 aux → reject
  assert.equal(register(prog(code, ex), [0, 65536]), null);
  assert.equal(register(prog(code, ex), [0, -1]), null);
  assert.equal(register(prog(code, { args: [16, 1], patches: ex.patches }), [0, 3]), null);
  // aux colour via rounding: 0.5 → 1
  assert.deepEqual(draw(register(prog(code, ex), [0, 0.5]), []).segs[0][4], 1);
});

test('patch into int imm: Q0 rule and range after patching', () => {
  const code = [{ op: 'REPEAT', imm: 0 }, { op: 'END' }]; // imm 0 is fine pre-patch
  const ex = { args: [0], patches: [[0, 0, 0]] };
  assert.ok(register(prog(code, ex), [255]));
  assert.ok(register(prog(code, ex), [0.6])); // rounds to 1
  assert.equal(register(prog(code, ex), [0.4]), null); // rounds to 0
  assert.equal(register(prog(code, ex), [256]), null);
  assert.equal(register(prog(code, { args: [1], patches: [[0, 0, 0]] }), [3]), null); // not Q0
  const sh = [{ op: 'SHIFT', dst: 0, a: 0, imm: 0 }];
  assert.ok(register(prog(sh, ex), [-31]));
  assert.equal(register(prog(sh, ex), [32]), null);
  const shh = [{ op: 'SHIFT_H', dst: 0, a: 0, imm: 0 }];
  assert.ok(register(prog(shh, ex), [14]));
  assert.equal(register(prog(shh, ex), [15]), null);
  assert.equal(register(prog(shh, ex), [-16]), null);
  // MUL aux s via patch: 62 ok, 63 reject; MUL.H 31/32
  const mul = [{ op: 'MUL', dst: 0, a: 0, b: 0, aux: 0 }];
  assert.ok(register(prog(mul, { args: [0], patches: [[0, 0, 1]] }), [62]));
  assert.equal(register(prog(mul, { args: [0], patches: [[0, 0, 1]] }), [63]), null);
  const mh = [{ op: 'MUL_H', dst: 0, a: 0, b: 0, aux: 0 }];
  assert.ok(register(prog(mh, { args: [0], patches: [[0, 0, 1]] }), [31]));
  assert.equal(register(prog(mh, { args: [0], patches: [[0, 0, 1]] }), [32]), null);
  // patch into an unused field must leave it 0
  const add = [{ op: 'ADD', dst: 0, a: 0, b: 0 }];
  assert.equal(register(prog(add, { args: [0], patches: [[0, 0, 0]] }), [1]), null);
  assert.equal(register(prog(add, { args: [0], patches: [[0, 0, 1]] }), [1]), null);
  // SET imm keeps a big value via clamp
  const big = register(prog([SET(0, 0)], { args: [16], patches: [[0, 0, 0]] }), [1e9]);
  assert.equal(draw(big, []).regs[0], MAX);
});

test('patch table checks', () => {
  const code = [SET(0, 0), SET(1, 0)];
  const ok = (patches, args = [0, 0]) => register(prog(code, { args: [0, 0], patches }), args);
  assert.ok(ok([[0, 0, 0], [1, 1, 0]]));
  assert.equal(ok([[1, 0, 0], [0, 1, 0]]), null); // not ascending
  assert.equal(ok([[0, 0, 0], [0, 1, 0]]), null); // same (instr, field) twice
  assert.equal(ok([[2, 0, 0]]), null); // instr ≥ n
  assert.equal(ok([[0, 2, 0]]), null); // arg ≥ declared
  assert.equal(ok([[0, 0, 2]]), null); // field
  assert.equal(ok([[0, 0, 0, 1]]), null); // reserved
  // (instr, imm) then (instr, aux) is ascending
  const p2 = [{ op: 'PLOT', a: 0, b: 0, aux: 0 }];
  // PLOT does not use imm: a patch there is judged on the patched value (§6.1
  // checks the instruction *after* patching), so 0 passes and 1 is rejected.
  assert.ok(register(prog(p2, { args: [0, 0], patches: [[0, 0, 0], [0, 1, 1]] }), [0, 7]));
  assert.equal(register(prog(p2, { args: [0, 0], patches: [[0, 0, 0], [0, 1, 1]] }), [1, 7]), null);
  assert.equal(register(prog(p2, { args: [0, 0], patches: [[0, 1, 1], [0, 0, 0]] }), [0, 7]), null); // aux before imm
});

test('arguments: count and finiteness', () => {
  const code = [SET(0, 0)];
  assert.ok(register(prog(code, { args: [0] }), [5]));
  assert.equal(register(prog(code, { args: [0] }), []), null);
  assert.equal(register(prog(code, { args: [0] }), [1, 2]), null);
  assert.equal(register(prog(code, { args: [0] }), [NaN]), null); // even when not patched
  assert.equal(register(prog(code, { args: [0] }), [-Infinity]), null);
});

// ---- registration: header and instruction checks -------------------------------

test('§6.1 header rejections', () => {
  const code = [SET(0, 1)];
  assert.ok(register(prog(code), []));
  rejects(code, { magic: 'SKI2' });
  rejects(code, { version: 2 });
  rejects(code, { kind: 1 });
  rejects(code, { reserved: 1 });
  rejects([], {});
  rejects(Array.from({ length: 65 }, () => SET(0, 0)));
  assert.ok(register(prog(Array.from({ length: 64 }, () => SET(0, 0))), []));
  rejects(code, { inputs: Array(9).fill(0) });
  rejects(code, { args: Array(9).fill(0) }, Array(9).fill(0));
  rejects(code, { inputs: [31] });
  rejects(code, { args: [31] }, [0]);
  assert.ok(register(prog(code, { inputs: [30], args: [30] }), [0]));
  rejects(code, { regions: [[0, 0, 1, 0]] }); // a declared region with none passed (§5.1)
  rejects(code, { inputs: [0], pad: 1 }); // padding must be 0
  const b = prog(code);
  assert.equal(register(b.subarray(0, b.length - 1), []), null);
  const longer = new Uint8Array(b.length + 4); longer.set(b);
  assert.equal(register(longer, []), null);
  const tooMany = Array.from({ length: 33 }, (_, i) => [i, 0, 0]);
  const c33 = Array.from({ length: 33 }, () => SET(0, 0));
  assert.equal(register(prog(c33, { args: [0], patches: tooMany }), [0]), null);
  assert.ok(register(prog(c33, { args: [0], patches: tooMany.slice(0, 32) }), [0]));
  rejects(code, { n: 2 }); // n disagrees with length
});

test('§6.1 instruction rejections', () => {
  for (const op of [14, 15, 23, 24, 25, 26, 27, 255]) rejects([{ op }]);
  rejects([{ op: 'SET', dst: 16 }]);
  rejects([{ op: 'ADD', dst: 0, a: 16, b: 0 }]);
  rejects([{ op: 'ADD', dst: 0, a: 0, b: 16 }]);
  rejects([{ op: 'SET', dst: 0, a: 1 }]); // unused a
  rejects([{ op: 'SET', dst: 0, b: 1 }]);
  rejects([{ op: 'SET', dst: 0, aux: 1 }]);
  rejects([{ op: 'SET', dst: 0, rsv: 1 }]); // reserved 2 bytes
  rejects([{ op: 'MOVE', dst: 1, a: 0, b: 0 }]); // MOVE has no dst
  rejects([{ op: 'MOVE', a: 0, b: 0, imm: 1 }]);
  rejects([{ op: 'NEG', dst: 0, a: 0, b: 1 }]);
  rejects([{ op: 'INPUT', dst: 0, a: 0 }]); // no inputs declared
  assert.ok(register(prog([{ op: 'INPUT', dst: 0, a: 1 }], { inputs: [0, 0] }), []));
  rejects([{ op: 'INPUT', dst: 0, a: 2 }], { inputs: [0, 0] });
  rejects([{ op: 'MUL', aux: 63 }]);
  rejects([{ op: 'MUL_H', aux: 32 }]);
  rejects([{ op: 'SHIFT', imm: 32 }]);
  rejects([{ op: 'SHIFT', imm: -32 }]);
  rejects([{ op: 'SHIFT_H', imm: 15 }]);
  rejects([{ op: 'SHIFT_H', imm: -16 }]);
  assert.ok(register(prog([{ op: 'SHIFT_H', imm: -15 }]), []));
  rejects([{ op: 'REPEAT', imm: 0 }, { op: 'END' }]);
  rejects([{ op: 'REPEAT', imm: 256 }, { op: 'END' }]);
  rejects([{ op: 'REPEAT', imm: 1, dst: 1 }, { op: 'END' }]);
  rejects([{ op: 'END' }]);
  rejects([{ op: 'END', a: 1 }]);
  rejects([{ op: 'REPEAT', imm: 1 }]);
  rejects([{ op: 'BREAK_IF_GT', a: 0, b: 1 }]);
  rejects([{ op: 'REPEAT', imm: 1 }, { op: 'END' }, { op: 'BREAK_IF_GT' }]);
  rejects([{ op: 'PLOT_C', dst: 16 }]);
  // nesting: 8 ok, 9 reject (REPEAT_REG counts too)
  const nest = (d) => [...Array.from({ length: d }, (_, i) => (i % 2 ? { op: 'REPEAT', imm: 1 } : { op: 'REPEAT_REG', a: 0 })),
    ...Array.from({ length: d }, () => ({ op: 'END' }))];
  assert.ok(register(prog(nest(8)), []));
  rejects(nest(9));
});

// ---- text contract --------------------------------------------------------------

test('CASE line round trip', () => {
  const bytes = assemble({ inputs: [16], args: [16], patches: [[0, 0, 0]],
    code: [SET(0, 0), { op: 'INPUT', dst: 1, a: 0 }, { op: 'MOVE', a: 0, b: 1 }] });
  const line = `CASE ${toHex(bytes)} ARGS 1 -0.5 INPUTS 2 3.25 NaN`;
  // -0.5 px rounds up to 0 (sec.4.5, v1.1); 3.25 px to 3.
  assert.equal(runCase(line),
    'RUN DONE 3 1 0 3 R -32768 212992 0 0 0 0 0 0 0 0 0 0 0 0 0 0 S 0');
  assert.equal(runCase(`CASE ${toHex(bytes)} ARGS 1 Infinity INPUTS 0`), 'REJECT');
  assert.equal(runCase(`CASE ${toHex(bytes)} ARGS 0 INPUTS 0`), 'REJECT');
  assert.equal(runCase(`CASE ${toHex(bytes)} ARGS 1 -0 INPUTS 1 -Infinity`).split(' ')[1], 'BADINPUT');
});

// ---- data regions (v1.3, §5) ------------------------------------------------------

// Region table rows are [type, q, k_rec, flags]; `regs` are makeRegion() objects.
const rg = (type, q, k, els) => { const g = makeRegion(type, q, k, els); assert.ok(g, 'bad region'); return g; };
const runD = (code, table, regs, args = [], inputs = []) => {
  const plan = register(prog(code, { regions: table, args: args.map(() => 0) }), args, regs);
  assert.ok(plan, 'expected registration to succeed');
  return draw(plan, inputs);
};
const rejectsD = (code, table, regs) =>
  assert.equal(register(prog(code, { regions: table }), [], regs), null, 'expected REJECT');
const EACH = (a, b = 0, aux = 0) => ({ op: 'EACH', a, b, aux });
const FETCH = (dst, a, b, m, k = 0) => ({ op: 'FETCH', dst, a, b, aux: m, imm: k });
const PUT = (dst, a, b, m, k = 0) => ({ op: 'PUT', dst, a, b, aux: m, imm: k });
const LOOKUP = (dst, a, b, g, k = 0) => ({ op: 'LOOKUP', dst, a, b, aux: g, imm: k });
const ADD = (dst, a, b) => ({ op: 'ADD', dst, a, b });
const END = { op: 'END' };
const I32T = [2, 0, 1, 0];

test('EACH over all records (aux=0) with FETCH', () => {
  const r = runD([EACH(0), FETCH(1, 0, 0, 1), ADD(2, 2, 1), END], [I32T], [rg(2, 0, 1, [10, 20, 30])]);
  // EACH 1 + 3 × (FETCH, ADD, END)
  assert.deepEqual([r.status, r.steps, r.regs[1], r.regs[2]], ['DONE', 10, 30, 60]);
});

test('EACH aux=1: N = r[b] read once; 0 skips; records ok; out of range INVALID', () => {
  const code = (n) => [SET(5, n), EACH(0, 5, 1), FETCH(1, 0, 0, 1), ADD(2, 2, 1), SET(5, 0), END, SET(7, 9)];
  const g = () => rg(2, 0, 1, [10, 20, 30]);
  const r2 = runD(code(2), [I32T], [g()]);
  // SET, EACH, 2 × (FETCH, ADD, SET, END), SET; changing r5 in the body does not cut the loop
  assert.deepEqual([r2.status, r2.steps, r2.regs[2], r2.regs[7]], ['DONE', 11, 30, 9]);
  const r0 = runD(code(0), [I32T], [g()]);
  assert.deepEqual([r0.status, r0.steps, r0.regs[1], r0.regs[2], r0.regs[7]], ['DONE', 3, 0, 0, 9]);
  const r3 = runD(code(3), [I32T], [g()]);
  assert.deepEqual([r3.status, r3.regs[2]], ['DONE', 60]);
  const r4 = runD(code(4), [I32T], [g()]);
  assert.deepEqual([r4.status, r4.steps, r4.regs[7]], ['INVALID', 2, 0]);
  const rn = runD(code(-1), [I32T], [g()]);
  assert.deepEqual([rn.status, rn.steps], ['INVALID', 2]);
  // N = 0 on the last loop of the program ends with DONE
  const rl = runD([SET(5, 0), EACH(0, 5, 1), FETCH(1, 0, 0, 1), END], [I32T], [g()]);
  assert.deepEqual([rl.status, rl.steps], ['DONE', 2]);
});

test('BREAK_IF_GT leaves an EACH and pops its frame', () => {
  const r = runD([SET(3, 4), EACH(0), FETCH(1, 0, 0, 1), { op: 'BREAK_IF_GT', a: 1, b: 3 }, ADD(2, 2, 1), END,
    SET(6, 1), SET(8, 1), EACH(0, 8, 1), FETCH(9, 0, 0, 1), END], [I32T], [rg(2, 0, 1, [1, 5, 2, 7])]);
  // SET, EACH, rec0 (FETCH, BRK, ADD, END), rec1 (FETCH, BRK taken), SET, SET, EACH, FETCH, END
  assert.deepEqual([r.status, r.steps, r.regs[2], r.regs[6], r.regs[9]], ['DONE', 13, 1, 1, 1]);
});

test('nested EACH of different regions reads both cursors', () => {
  const r = runD([EACH(0), EACH(1), FETCH(1, 0, 0, 1), FETCH(2, 1, 0, 2), ADD(4, 4, 1), ADD(4, 4, 2),
    ADD(6, 6, 3), END, END], [I32T, [2, 0, 2, 0]], [rg(2, 0, 1, [1, 2]), rg(2, 0, 2, [10, 30, 20, 40])]);
  // v1.4 field by field: records (10, 20) and (30, 40) listed as 10 30 | 20 40.
  // (1+10)+(1+30)+(2+10)+(2+30) = 86; 2·(20+40) = 120. Steps 1 + 2·(1 + 2·6 + 1).
  assert.deepEqual([r.status, r.steps, r.regs[4], r.regs[6]], ['DONE', 29, 86, 120]);
});

test('self-nested EACH of one region: FETCH resolves the innermost lexical EACH', () => {
  const r = runD([EACH(0), FETCH(1, 0, 0, 1), EACH(0), FETCH(2, 0, 0, 1), { op: 'MUL', dst: 4, a: 1, b: 2, aux: 0 },
    ADD(5, 5, 4), END, FETCH(6, 0, 0, 1), ADD(7, 7, 6), END], [I32T], [rg(2, 0, 1, [3, 4])]);
  // pairs (3,3)(3,4)(4,3)(4,4): 9+12+12+16 = 49; the FETCH after the inner END reads the outer: 3+4
  assert.deepEqual([r.status, r.regs[5], r.regs[7]], ['DONE', 49, 7]);
});

test('FETCH: u16 ≥ 32768 unsigned, i16 negative, ±k, saturation', () => {
  const r = runD([EACH(0), EACH(1),
    FETCH(0, 0, 0, 2), // 40000, 65535
    FETCH(2, 0, 0, 1, 16), // 40000·2^16 > 2^31 → MAX
    FETCH(3, 0, 1, 1, -1), // ⌊65535/2⌋
    FETCH(4, 1, 0, 1, -1), // ⌊−3/2⌋ = −2
    FETCH(5, 1, 1, 1, 16), // −2^15·2^16 = −2^31 exactly
    FETCH(6, 1, 1, 1, 17), // −2^32 → MIN
    FETCH(7, 1, 2, 2), // 32767, 5
    FETCH(9, 1, 0, 1, -31), // ⌊−3/2^31⌋ = −1
    FETCH(10, 0, 2, 1, 31), // 1·2^31 → MAX
    FETCH(11, 0, 3, 1, 31), // 0
    END, END], [[1, 0, 4, 0], [0, 0, 4, 0]],
  [rg(1, 0, 4, [40000, 65535, 1, 0]), rg(0, 0, 4, [-3, -32768, 32767, 5])]);
  assert.equal(r.status, 'DONE');
  assert.deepEqual(Array.from(r.regs.slice(0, 12)),
    [40000, 65535, MAX, 32767, -2, MIN, MIN, 32767, 5, -1, MAX, 0]);
});

test('PUT: i16 / u16 saturation, i32 passthrough, k shift', () => {
  const a = rg(0, 0, 3, [0, 0, 0]), b = rg(1, 0, 3, [0, 0, 0]), c = rg(2, 0, 2, [0, 0]), d = rg(2, 0, 2, [0, 0]);
  const r = runD([SET(0, 40000), SET(1, -40000), SET(2, -5), SET(3, 3), SET(4, MAX),
    EACH(0), PUT(0, 0, 0, 3), END,
    EACH(1), PUT(0, 1, 0, 3), END,
    EACH(2), PUT(1, 2, 0, 2), END,
    EACH(3), PUT(3, 3, 0, 2, 1), END], // 3·2 = 6; MAX·2 → MAX
  [[0, 0, 3, 1], [1, 0, 3, 1], [2, 0, 2, 1], [2, 0, 2, 1]], [a, b, c, d]);
  assert.equal(r.status, 'DONE');
  assert.deepEqual(Array.from(a.mem), [32767, -32768, -5]);
  assert.deepEqual(Array.from(b.mem), [40000, 0, 0]);
  assert.deepEqual(Array.from(c.mem), [-40000, -5]);
  assert.deepEqual(Array.from(d.mem), [6, MAX]);
  // u16 with k < 0: ⌊3/2⌋ = 1, ⌊MAX/2⌋ → 65535; i16 ⌊−5/4⌋ = −2
  const e = rg(1, 0, 2, [9, 9]), f = rg(0, 0, 1, [9]);
  runD([SET(3, 3), SET(4, MAX), SET(5, -5), EACH(0), PUT(3, 0, 0, 2, -1), END, EACH(1), PUT(5, 1, 0, 1, -2), END],
    [[1, 0, 2, 1], [0, 0, 1, 1]], [e, f]);
  assert.deepEqual([...e.mem, ...f.mem], [1, 65535, -2]);
});

test('PUT then FETCH in the same draw, and in a later loop; b offset keeps other elements', () => {
  const g = rg(2, 0, 2, [1, 3, 2, 4]); // records (1, 2), (3, 4), field by field (v1.4)
  const r = runD([SET(9, 100), EACH(0), FETCH(0, 0, 0, 1), ADD(1, 0, 9), PUT(1, 0, 1, 1), FETCH(2, 0, 1, 1),
    ADD(3, 3, 2), END, EACH(0), FETCH(4, 0, 1, 1), ADD(5, 5, 4), END], [[2, 0, 2, 1]], [g]);
  assert.deepEqual([r.status, r.regs[3], r.regs[5]], ['DONE', 204, 204]);
  assert.deepEqual(Array.from(g.mem), [1, 3, 101, 103]);
});

test('LOOKUP: clamp both ends, g = 0 / 16 / 31, k, u16, dst == b', () => {
  const r = runD([
    SET(0, -1), LOOKUP(1, 0, 0, 0), // index −1 → 0 → −7
    SET(0, 2), LOOKUP(2, 0, 0, 0), // 200
    SET(0, 4), LOOKUP(3, 0, 0, 0), // past n − 1 → 3 → 300
    SET(0, MAX), LOOKUP(4, 0, 0, 31), // ⌊MAX/2^31⌋ = 0 → −7
    SET(0, MIN), LOOKUP(5, 0, 0, 31, 1), // ⌊MIN/2^31⌋ = −1 → 0 → −14
    SET(0, 3 << 16), LOOKUP(6, 0, 0, 16, 1), // 3 → 600
    SET(0, -65536), LOOKUP(7, 0, 0, 16, -1), // −1 → 0 → ⌊−7/2⌋ = −4
    SET(0, 196607), LOOKUP(8, 0, 0, 16), // ⌊2.99…⌋ = 2 → 200
    SET(9, 1), LOOKUP(9, 0, 9, 0), // reads r9 before writing it → 100
    LOOKUP(10, 1, 0, 0), // u16 65535 (n = 1: any index → 0)
    LOOKUP(11, 1, 0, 0, 16), // 65535·2^16 → MAX
    SET(0, MIN), LOOKUP(12, 0, 0, 0), // most negative index → 0
  ], [[0, 0, 1, 0], [1, 0, 1, 0]], [rg(0, 0, 1, [-7, 100, 200, 300]), rg(1, 0, 1, [65535])]);
  assert.equal(r.status, 'DONE');
  assert.deepEqual(Array.from(r.regs.slice(1, 13)), [-7, 200, 300, -7, -14, 600, -4, 200, 100, 65535, MAX, -7]);
});

test('LIMIT in the middle of an EACH keeps earlier PUTs', () => {
  const g = rg(2, 0, 1, [0, 0, 0]);
  const r = runD([SET(0, 1), SET(1, 1), EACH(0), PUT(0, 0, 0, 1), ADD(0, 0, 1),
    { op: 'REPEAT', imm: 125 }, { op: 'REPEAT', imm: 19 }, SET(2, 7), END, END, END], [[2, 0, 1, 1]], [g]);
  // rec0: PUT at step 4, burn 5,001, END 5,007; rec1: PUT at 5,008, burn passes 10,000
  assert.deepEqual([r.status, r.steps, r.regs[0]], ['LIMIT', 10000, 3]);
  assert.deepEqual(Array.from(g.mem), [1, 2, 0]);
});

test('aliased regions share memory', () => {
  const g = rg(2, 0, 1, [5, 6]);
  const table = [[2, 0, 1, 1], [2, 0, 1, 0]];
  const code = [SET(0, 9), SET(7, 1), EACH(0, 7, 1), PUT(0, 0, 0, 1), END, LOOKUP(1, 1, 2, 0),
    EACH(1), FETCH(3, 1, 0, 1), ADD(4, 4, 3), END];
  const plan = register(prog(code, { regions: table }), [], [g, g]);
  const r = draw(plan, []);
  assert.deepEqual([r.status, r.regs[1], r.regs[4]], ['DONE', 9, 15]);
  assert.equal(format(r, [g, g]).split(' D ')[1], '2 2 9 6 2 9 6');
  // An alias carries its target's type/q/k_rec, so a mismatching entry rejects.
  assert.equal(register(prog(code, { regions: [[2, 0, 1, 1], [2, 1, 1, 0]] }), [], [g, g]), null);
});

test('region descriptors (shiki.data model)', () => {
  assert.equal(makeRegion(2, 0, 1, []), null); // 0 records
  assert.equal(makeRegion(2, 0, 2, [1, 2, 3]), null); // n not a multiple of k_rec
  assert.ok(makeRegion(1, 30, 1, new Array(65535).fill(0)));
  assert.equal(makeRegion(1, 0, 1, new Array(65536).fill(0)), null);
  assert.ok(makeRegion(1, 0, 8, new Array(8 * 65535).fill(0))); // 65,535 records of 8
  assert.equal(makeRegion(3, 0, 1, [0]), null);
  assert.equal(makeRegion(0, 31, 1, [0]), null);
  assert.equal(makeRegion(0, 0, 0, [0]), null);
  assert.equal(makeRegion(0, 0, 9, new Array(9).fill(0)), null);
  assert.equal(makeRegion(0, 0, 1, [32768]), null);
  assert.equal(makeRegion(0, 0, 1, [-32769]), null);
  assert.equal(makeRegion(1, 0, 1, [-1]), null);
  assert.equal(makeRegion(1, 0, 1, [65536]), null);
  assert.equal(makeRegion(2, 0, 1, [MAX + 1]), null);
  assert.equal(makeRegion(2, 0, 1, [1.5]), null);
  assert.ok(makeRegion(2, 0, 1, [MIN, MAX]));
});

test('§6.1 / §5.1 region registration rejects', () => {
  const g1 = () => rg(2, 0, 1, [1, 2]);
  const ok = [EACH(0), FETCH(0, 0, 0, 1), END];
  assert.ok(register(prog(ok, { regions: [I32T] }), [], [g1()]));
  // count mismatch
  rejectsD(ok, [I32T], []);
  rejectsD(ok, [I32T], [g1(), g1()]);
  rejectsD([SET(0, 0)], [], [g1()]);
  // type / q / k_rec mismatch
  rejectsD(ok, [[1, 0, 1, 0]], [g1()]);
  rejectsD(ok, [[2, 1, 1, 0]], [g1()]);
  rejectsD(ok, [[2, 0, 2, 0]], [g1()]);
  // table values: type, q, k_rec, flags (bit0 only)
  rejectsD(ok, [[3, 0, 1, 0]], [g1()]);
  rejectsD(ok, [[2, 31, 1, 0]], [g1()]);
  rejectsD(ok, [[2, 0, 0, 0]], [g1()]);
  rejectsD(ok, [[2, 0, 9, 0]], [g1()]);
  rejectsD(ok, [[2, 0, 1, 2]], [g1()]);
  rejectsD(ok, [[2, 0, 1, 3]], [g1()]);
  assert.ok(register(prog(ok, { regions: [[2, 0, 1, 1]] }), [], [g1()])); // write flag alone is fine
  rejectsD(ok, Array(5).fill(I32T), Array(5).fill(g1())); // more than 4 regions
  // region number ≥ declared
  rejectsD([EACH(1), FETCH(0, 1, 0, 1), END], [I32T], [g1()]);
  rejectsD([LOOKUP(0, 1, 0, 0)], [I32T], [g1()]);
  // PUT needs the write flag
  rejectsD([EACH(0), PUT(0, 0, 0, 1), END], [I32T], [g1()]);
  assert.ok(register(prog([EACH(0), PUT(0, 0, 0, 1), END], { regions: [[2, 0, 1, 1]] }), [], [g1()]));
  // LOOKUP needs k_rec = 1
  const g2 = rg(2, 0, 2, [1, 2]);
  rejectsD([LOOKUP(0, 0, 0, 0)], [[2, 0, 2, 0]], [g2]);
  assert.ok(register(prog([LOOKUP(0, 0, 0, 31, -31)], { regions: [I32T] }), [], [g1()]));
  rejectsD([LOOKUP(0, 0, 0, 32)], [I32T], [g1()]);
  rejectsD([LOOKUP(0, 0, 16, 0)], [I32T], [g1()]);
  rejectsD([LOOKUP(16, 0, 0, 0)], [I32T], [g1()]);
  rejectsD([LOOKUP(0, 0, 0, 0, 32)], [I32T], [g1()]);
  rejectsD([LOOKUP(0, 0, 0, 0, -32)], [I32T], [g1()]);
  // FETCH / PUT outside an EACH of the same region (inside a REPEAT or another region's EACH)
  rejectsD([FETCH(0, 0, 0, 1)], [I32T], [g1()]);
  rejectsD([{ op: 'REPEAT', imm: 1 }, FETCH(0, 0, 0, 1), END], [I32T], [g1()]);
  rejectsD([EACH(1), FETCH(0, 0, 0, 1), END], [I32T, I32T], [g1(), g1()]);
  rejectsD([EACH(0), END, FETCH(0, 0, 0, 1)], [I32T], [g1()]);
  // inside a REPEAT inside the EACH is fine
  assert.ok(register(prog([EACH(0), { op: 'REPEAT', imm: 2 }, FETCH(0, 0, 0, 1), END, END], { regions: [I32T] }), [], [g1()]));
  // b + m ≤ k_rec, m ≥ 1, dst + m − 1 ≤ 15, k range
  const g4 = () => rg(2, 0, 4, [1, 2, 3, 4]);
  const T4 = [[2, 0, 4, 1]];
  assert.ok(register(prog([EACH(0), FETCH(12, 0, 0, 4), PUT(13, 0, 1, 3), END], { regions: T4 }), [], [g4()]));
  rejectsD([EACH(0), FETCH(0, 0, 1, 4), END], T4, [g4()]);
  rejectsD([EACH(0), PUT(0, 0, 4, 1), END], T4, [g4()]);
  rejectsD([EACH(0), FETCH(0, 0, 0, 0), END], T4, [g4()]);
  rejectsD([EACH(0), FETCH(14, 0, 0, 3), END], T4, [g4()]);
  rejectsD([EACH(0), PUT(13, 0, 0, 4), END], T4, [g4()]);
  rejectsD([EACH(0), FETCH(0, 0, 0, 1, 32), END], T4, [g4()]);
  rejectsD([EACH(0), PUT(0, 0, 0, 1, -32), END], T4, [g4()]);
  // EACH fields: aux 0..3 (v2.0 draft: bit 1 the start register in imm);
  // bit 0 clear ⇒ b = 0; bit 0 set ⇒ b ≤ 15; dst = 0; bit 1 clear ⇒ imm = 0
  rejectsD([EACH(0, 1, 0), END], [I32T], [g1()]);
  rejectsD([EACH(0, 0, 4), END], [I32T], [g1()]);
  rejectsD([EACH(0, 16, 1), END], [I32T], [g1()]);
  assert.ok(register(prog([EACH(0, 15, 1), END], { regions: [I32T] }), [], [g1()]));
  rejectsD([{ ...EACH(0), dst: 1 }, END], [I32T], [g1()]);
  rejectsD([{ ...EACH(0), imm: 1 }, END], [I32T], [g1()]);
  rejectsD([EACH(0)], [I32T], [g1()]); // no END
  rejectsD([{ ...LOOKUP(0, 0, 0, 0), rsv: 1 }], [I32T], [g1()]);
  // EACH counts toward the 8-deep stack
  const nest = (d) => [...Array.from({ length: d }, (_, i) => (i % 2 ? { op: 'REPEAT', imm: 1 } : EACH(0))),
    ...Array.from({ length: d }, () => END)];
  assert.ok(register(prog(nest(8), { regions: [I32T] }), [], [g1()]));
  rejectsD(nest(9), [I32T], [g1()]);
  // BREAK_IF_GT inside an EACH is inside a loop
  assert.ok(register(prog([EACH(0), { op: 'BREAK_IF_GT', a: 0, b: 1 }, END], { regions: [I32T] }), [], [g1()]));
});

test('patches into data-region ops: k and m are Q0, checked after patching', () => {
  const T4 = [[2, 0, 4, 0]];
  const g4 = () => rg(2, 0, 4, [1, 2, 3, 4]);
  const code = [EACH(0), FETCH(0, 0, 0, 1, 0), END];
  const reg = (argQ, patches, args) => register(prog(code, { regions: T4, args: argQ, patches }), args, [g4()]);
  const plan = reg([0, 0], [[1, 0, 0], [1, 1, 1]], [-1, 3]); // k = −1, m = 3
  assert.ok(plan);
  assert.deepEqual(Array.from(draw(plan, []).regs.slice(0, 3)), [0, 1, 1]); // ⌊1/2⌋, ⌊2/2⌋, ⌊3/2⌋
  assert.equal(reg([1, 0], [[1, 0, 0], [1, 1, 1]], [0, 3]), null); // k not Q0
  assert.equal(reg([0, 0], [[1, 0, 0], [1, 1, 1]], [0, 5]), null); // m = 5: b + m > k_rec
  assert.equal(reg([0, 0], [[1, 0, 0], [1, 1, 1]], [32, 1]), null); // k = 32
  // v2.0 draft: EACH's imm is the start register (an int field): Q0 only,
  // and 0 unless aux bit 1 is set.
  assert.equal(register(prog(code, { regions: T4, args: [16], patches: [[0, 0, 0]] }), [0], [g4()]), null);
  assert.ok(register(prog(code, { regions: T4, args: [0], patches: [[0, 0, 0]] }), [0], [g4()]));
  assert.equal(register(prog(code, { regions: T4, args: [0], patches: [[0, 0, 0]] }), [1], [g4()]), null);
});

// ---- text contract: DATA / ALIAS / D ------------------------------------------------

test('CASE DATA / ALIAS parse and D output', () => {
  const bytes = assemble({ regions: [[0, 0, 1, 1], [0, 0, 1, 0]],
    code: [LOOKUP(0, 1, 15, 0), SET(3, -100000), EACH(0), PUT(3, 0, 0, 1), END] });
  const hex = toHex(bytes);
  const line = `CASE ${hex} ARGS 0 INPUTS 0 DATA 2 0 0 1 2 -5 7 ALIAS 0`;
  const c = parseCase(line);
  assert.equal(c.regions.length, 2);
  assert.equal(c.regions[0], c.regions[1]); // same memory
  assert.deepEqual(Array.from(c.regions[0].mem), [-5, 7]);
  // LOOKUP via the alias reads −5, then PUT −100000 saturates to −32768 in both records
  assert.equal(runCase(line),
    'RUN DONE 7 0 0 0 R -5 0 0 -100000 0 0 0 0 0 0 0 0 0 0 0 0 S 0 D 2 2 -32768 -32768 2 -32768 -32768');
  // invalid descriptors make the whole case REJECT
  assert.equal(runCase(`CASE ${hex} ARGS 0 INPUTS 0 DATA 2 0 0 1 2 32768 7 ALIAS 0`), 'REJECT');
  assert.equal(runCase(`CASE ${hex} ARGS 0 INPUTS 0 DATA 2 0 31 1 2 -5 7 ALIAS 0`), 'REJECT');
  assert.equal(runCase(`CASE ${hex} ARGS 0 INPUTS 0 DATA 2 0 0 9 1 1 ALIAS 0`), 'REJECT'); // k_rec 9 (v1.4: <R> is a record count, so 'not whole records' cannot be written)
  assert.equal(runCase(`CASE ${hex} ARGS 0 INPUTS 0 DATA 2 0 0 1 0 ALIAS 0`), 'REJECT'); // 0 records
  assert.equal(runCase(`CASE ${hex} ARGS 0 INPUTS 0 DATA 2 ALIAS 0 0 0 1 1 1`), 'REJECT'); // alias forward
  assert.equal(runCase(`CASE ${hex} ARGS 0 INPUTS 0 DATA 2 0 0 1 1 1 ALIAS 1`), 'REJECT'); // alias to itself
  // registration failures: count, flags of the target via the alias
  assert.equal(runCase(`CASE ${hex} ARGS 0 INPUTS 0 DATA 1 0 0 1 1 1`), 'REJECT');
  assert.equal(runCase(`CASE ${hex} ARGS 0 INPUTS 0`), 'REJECT');
  assert.equal(runCase(`CASE ${hex} ARGS 0 INPUTS 0 DATA 2 1 0 1 1 1 ALIAS 0`), 'REJECT'); // u16 ≠ i16
  // two separate regions: only the first is written
  assert.equal(runCase(`CASE ${hex} ARGS 0 INPUTS 0 DATA 2 0 0 1 1 4 0 0 1 3 1 2 3`),
    'RUN DONE 5 0 0 0 R 1 0 0 -100000 0 0 0 0 0 0 0 0 0 0 0 0 S 0 D 2 1 -32768 3 1 2 3');
  // u16 prints unsigned; BADINPUT prints D unchanged
  const ub = toHex(assemble({ inputs: [0], regions: [[1, 0, 1, 1]], code: [EACH(0), PUT(0, 0, 0, 1), END] }));
  assert.equal(runCase(`CASE ${ub} ARGS 0 INPUTS 1 NaN DATA 1 1 0 1 2 65535 40000`),
    'RUN BADINPUT 0 0 0 0 R 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 S 0 D 1 2 65535 40000');
  assert.equal(runCase(`CASE ${ub} ARGS 0 INPUTS 0 DATA 1 1 0 1 2 65535 40000`).split(' D ')[1], '1 2 0 0');
  // no DATA → no D; DATA 0 behaves as no regions (and no D)
  const plain = toHex(assemble({ code: [SET(0, 1)] }));
  assert.equal(runCase(`CASE ${plain} ARGS 0 INPUTS 0`), 'RUN DONE 1 0 0 0 R 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 S 0');
  assert.equal(runCase(`CASE ${plain} ARGS 0 INPUTS 0 DATA 0`), 'RUN DONE 1 0 0 0 R 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 S 0');
  // i32 extremes round-trip
  const i32 = toHex(assemble({ regions: [I32T], code: [LOOKUP(0, 0, 0, 0)] }));
  assert.equal(runCase(`CASE ${i32} ARGS 0 INPUTS 0 DATA 1 2 0 1 2 -2147483648 2147483647`),
    'RUN DONE 1 0 0 0 R -2147483648 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 S 0 D 1 2 -2147483648 2147483647');
  // malformed lines throw
  assert.throws(() => parseCase(`CASE ${plain} ARGS 0 INPUTS 0 DATA 1 2 0 1 1`));
  assert.throws(() => parseCase(`CASE ${plain} ARGS 0 INPUTS 0 DATA 1 2 0 1 1 5 extra`));
  assert.throws(() => parseCase(`CASE ${plain} ARGS 0 INPUTS 0 DATA 1 2 0 1 1 x`));
});

// ---- v2.0 draft (docs/kasane/shiki-v2.md sec.3, 4) --------------------------------
const LOAD = (dst, a, b, c, m, k = 0) => ({ op: 'LOAD', dst, a, b, aux: c | m << 8, imm: k });
const STORE = (dst, a, b, c, m, k = 0) => ({ op: 'STORE', dst, a, b, aux: c | m << 8, imm: k });

test('LOAD / STORE: fields c..c+m-1 of record r[b]; out of 0..R-1 INVALID', () => {
  // i16, k_rec 3, R 3 (fields: [1,2,3], [10,20,30], [100,200,300]), writable.
  const T = [[0, 0, 3, 1]], g = rg(0, 0, 3, [1, 2, 3, 10, 20, 30, 100, 200, 300]);
  const r = runD([SET(0, 2), LOAD(4, 0, 0, 1, 2), SET(1, 1), STORE(4, 0, 1, 0, 2, 1)], T, [g]);
  assert.deepEqual([r.status, r.regs[4], r.regs[5]], ['DONE', 30, 300]);
  // STORE of record 1, fields 0..1, shifted left by 1.
  assert.deepEqual(Array.from(g.mem), [1, 60, 3, 10, 600, 30, 100, 200, 300]);
  for (const bad of [-1, 3]) {
    const x = runD([SET(0, bad), LOAD(4, 0, 0, 0, 1)], T, [rg(0, 0, 3, [1, 2, 3, 10, 20, 30, 100, 200, 300])]);
    assert.deepEqual([x.status, x.steps], ['INVALID', 2]);
  }
  // STORE saturates to the element type; the values are all read before any write.
  const h = rg(0, 0, 3, [0, 0, 0, 0, 0, 0, 0, 0, 0]);
  runD([SET(2, 70000), SET(3, -5), SET(0, 0), STORE(2, 0, 0, 1, 2)], T, [h]);
  assert.deepEqual(Array.from(h.mem), [0, 0, 0, 32767, 0, 0, -5, 0, 0]);
  rejectsD([STORE(0, 0, 0, 0, 1)], [[0, 0, 3, 0]], [rg(0, 0, 3, [1, 2, 3])]);   // read-only region
  rejectsD([LOAD(0, 0, 0, 2, 2)], T, [rg(0, 0, 3, [1, 2, 3])]);                 // c + m > k_rec
  rejectsD([LOAD(0, 0, 0, 0, 0)], T, [rg(0, 0, 3, [1, 2, 3])]);                 // m = 0
  rejectsD([LOAD(14, 0, 0, 0, 3)], T, [rg(0, 0, 3, [1, 2, 3])]);                // dst + m > 16
});

test('DRAW_INDEX reads the draw number (0 by default; INDEX on a CASE line)', () => {
  const bytes = assemble({ code: [{ op: 'DRAW_INDEX', dst: 3 }] });
  assert.equal(draw(register(bytes), [], 7).regs[3], 7);
  assert.equal(draw(register(bytes)).regs[3], 0);
  assert.equal(runCase(`CASE ${toHex(bytes)} ARGS 0 INPUTS 0 INDEX 255`).split(' ')[10], '255');
  assert.equal(register(assemble({ code: [{ op: 'DRAW_INDEX', dst: 3, a: 1 }] })), null);
});

test('EACH from a start record r[imm] (aux bit 1), with and without a count', () => {
  const g = () => rg(2, 0, 1, [10, 20, 30, 40, 50]);
  const each = (aux, imm, b = 0) => ({ op: 'EACH', a: 0, b, aux, imm });
  // start 2, the rest: 30 + 40 + 50
  let r = runD([SET(6, 2), each(2, 6), FETCH(1, 0, 0, 1), ADD(2, 2, 1), END], [I32T], [g()]);
  assert.deepEqual([r.status, r.regs[2]], ['DONE', 120]);
  // start 1, count 2: 20 + 30
  r = runD([SET(6, 1), SET(5, 2), each(3, 6, 5), FETCH(1, 0, 0, 1), ADD(2, 2, 1), END], [I32T], [g()]);
  assert.deepEqual([r.status, r.regs[2]], ['DONE', 50]);
  // start R: nothing, DONE; start R + 1 or -1: INVALID; count past R - start: INVALID
  r = runD([SET(6, 5), each(2, 6), FETCH(1, 0, 0, 1), ADD(2, 2, 1), END, SET(7, 1)], [I32T], [g()]);
  assert.deepEqual([r.status, r.regs[2], r.regs[7]], ['DONE', 0, 1]);
  for (const st of [6, -1]) assert.equal(runD([SET(6, st), each(2, 6), END], [I32T], [g()]).status, 'INVALID');
  assert.equal(runD([SET(6, 3), SET(5, 3), each(3, 6, 5), END], [I32T], [g()]).status, 'INVALID');
  // the flag bit 1 needs a register in imm; without it imm must be 0
  rejectsD([each(0, 1), END], [I32T], [g()]);
  rejectsD([each(2, 16), END], [I32T], [g()]);
  rejectsD([each(4, 0), END], [I32T], [g()]);
});

test('OPS names cover the v1 table', () => {
  assert.equal(OPS.SHIFT_H, 22);
  assert.equal(OPS.LOOKUP, 26);
  assert.equal(OPS.DRAW_INDEX, 29);
});

console.log(`${passed} passed, ${failed} failed`);
if (failed) process.exit(1);
