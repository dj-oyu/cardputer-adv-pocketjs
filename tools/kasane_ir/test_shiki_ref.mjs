// Hand-built edge cases for the Shiki v1 reference engine (shiki_ref.mjs).
// Each expectation is derived from docs/kasane/shiki-spec.md by hand, not by
// running the engine, so a failure here is a disagreement with the spec text.
// Run: node tools/kasane_ir/test_shiki_ref.mjs

import assert from 'node:assert/strict';
import {
  assemble, register, draw, format, runCase, toHex, SIN_TABLE, sinQ16, toFixed,
  mul32, shift32, mulH, shiftH, addH, subH, OPS,
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

test('coordinate floor of negatives and range edges', () => {
  const r = run([SET(0, -1), SET(1, -480 * 65536), { op: 'PLOT', a: 0, b: 1, aux: 7 }]);
  assert.equal(r.status, 'DONE');
  assert.deepEqual(r.segs, [[-1, -480, -1, -480, 7]]);
  assert.deepEqual([r.pen.valid, r.pen.x, r.pen.y], [true, -1, -480]);
  // −480·65536 − 1 → −481 → INVALID
  const r2 = run([SET(0, -480 * 65536 - 1), SET(1, 0), { op: 'PLOT', a: 0, b: 1 }]);
  assert.equal(r2.status, 'INVALID');
  assert.equal(r2.steps, 3);
  assert.equal(r2.pen.valid, false);
  // 720·65536 + 65535 → 720 ok; 721·65536 → INVALID
  const r3 = run([SET(0, 720 * 65536 + 65535), { op: 'MOVE', a: 0, b: 0 }]);
  assert.deepEqual([r3.status, r3.pen.x, r3.pen.y], ['DONE', 720, 720]);
  const r4 = run([SET(0, 721 * 65536), SET(1, 0), { op: 'MOVE', a: 1, b: 0 }]);
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
  rejects(code, { regions: [[0, 0, 1, 0]] }); // stage 1: no data regions
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
  assert.equal(runCase(line),
    'RUN DONE 3 1 -1 3 R -32768 212992 0 0 0 0 0 0 0 0 0 0 0 0 0 0 S 0');
  assert.equal(runCase(`CASE ${toHex(bytes)} ARGS 1 Infinity INPUTS 0`), 'REJECT');
  assert.equal(runCase(`CASE ${toHex(bytes)} ARGS 0 INPUTS 0`), 'REJECT');
  assert.equal(runCase(`CASE ${toHex(bytes)} ARGS 1 -0 INPUTS 1 -Infinity`).split(' ')[1], 'BADINPUT');
});

test('OPS names cover the v1 table', () => {
  assert.equal(OPS.SHIFT_H, 22);
  assert.equal(OPS.LOOKUP, 26);
});

console.log(`${passed} passed, ${failed} failed`);
if (failed) process.exit(1);
