// The compiler's own cases that DERBY's plans do not reach (host, Node):
//   node tools/kasane_ir/test_kir.mjs
// Output equality with the hand IR is tools/kasane_ir/check_equivalence.py.
import assert from 'node:assert/strict';
import {compile, parseText} from './kir.mjs';

const ops = src => compile(src).text;
// Folding only where the VM would compute the same float: (a+1)+2 stays two
// ADDs, a+(1+2) is one; sin of a constant is never folded (sinf vs Math.sin).
assert.equal(compile('input a = 0; plot(a + 1 + 2, 0, 1);').count, 6);
assert.equal(compile('input a = 0; plot(a + (1 + 2), 0, 1);').count, 4);
assert.match(ops('plot(sin(1), 0, 1);'), /N/);
// Common subexpressions share one register; a write to a variable ends that.
assert.equal(compile('input a = 0, b = 1; plot(a * b, a * b, 1);').count, 4);
assert.equal(compile('input a = 0; let x = a; x += 1; plot(x * 2, 0, 1); x += 1; plot(x * 2, 0, 1);').count, 9);
// x/4 is x*.25 exactly; x/3 is not and says so.
assert.deepEqual(compile('input a = 0; plot(a / 4, 0, 1);').warnings, []);
assert.equal(compile('input a = 0; plot(a / 3, 0, 1);').warnings.length, 1);
// Dead code goes; the zero every register starts with costs no SET.
assert.equal(compile('input a = 0; let y = a * 3; plot(a, 0, 1);').count, 2);
assert.equal(compile('let x = 0; repeat (4) { x += 1; plot(x, x, 1); }').count, 5);
// A loop count from a register, a break, colours from registers.
assert.match(ops('input n = 0, c = 1; let i = 0; repeat (n) { i += 1; if (i > 3) break; plot(i, i, c); }'), /Q.*B.*p/);
// Seventeen values live at once cannot fit in sixteen registers.
const many = Array.from({length: 17}, (_, k) => `const v${k} = sin(in(${k % 8}) + ${k});`).join('\n') +
  '\nplot(' + Array.from({length: 17}, (_, k) => `v${k}`).join(' + ') + ', 0, 1);';
assert.throws(() => compile(many), /more than 16 values live/);
// Constants and inputs are re-read inside a loop when registers run short.
const tight = Array.from({length: 15}, (_, k) => `let v${k} = sin(in(${k % 8}) + ${k});`).join('\n') +
  '\nrepeat (3) {\n' + Array.from({length: 15}, (_, k) => `  v${k} = v${k} * ${k + 2} + ${k + 40};`).join('\n') + '\n}\n' +
  'plot(' + Array.from({length: 15}, (_, k) => `v${k}`).join(' + ') + ', 0, 1);';
assert.ok(compile(tight).rematerialised.length > 0);
// CUBIC reads r0..r7: the points land there, a repeated value in its own register.
const cubic = compile('input a = 0, b = 1; cubic(8, 65535, a, b, a, b, 10, 20, 10, 20);');
const set = new Map(parseText(cubic.text).filter(i => i.op <= 1).map(i => [i.dst, i]));
for (let r = 0; r < 8; ++r) assert.ok(set.has(r), `r${r} set before CUBIC`);
// linePattern: colour A and the period are immediates; its six values sit in
// r10..r15 (one SET or copy before the call for a slot that sees several
// values, the value itself for a slot that always sees one).
const lp = compile(`input x = 0, y = 1, u = 2;
let v = u;
repeat (3) {
  move(x, y); linePattern(x + 10, y, 5, 4660, -1, v, v + 7, 1, 1, 24);
  move(x, y + 1); linePattern(x + 10, y + 1, 6, 4661, -1, v, v + 7, 1, 1, 12);
  v += 3;
}`);
const pi = parseText(lp.text).filter(i => i.op === 15);
assert.equal(pi.length, 2);
assert.deepEqual(pi.map(i => [i.dst, i.value, i.color]), [[10, 24, 4660], [10, 12, 4661]]);
assert.match(lp.text, /S10,5 .*X10,.*S10,6 .*X10,/);   // the pattern slot: a SET each call
assert.throws(() => compile('linePattern(1, 2, 3, 4, 5, 6, 7, 8, 9, 25);'), /period is 1\.\.24/);
assert.throws(() => compile('input a = 0; linePattern(1, 2, 3, a, 5, 6, 7, 8, 9, 24);'), /colorA must be/);
// The nibble form escapes op 15 (15 is also the INPUT macro) and decodes back.
{
  const {encodeNibble, DECODER_NIBBLE} = await import('./pack.mjs');
  const {assemble} = await import('./kir.mjs');
  const dec = new Function(DECODER_NIBBLE + 'return prog;')();
  const code = parseText(lp.text);
  assert.deepEqual(dec(encodeNibble(lp.text), []), assemble(code, []));
}
// A too-long plan is reported, not truncated.
assert.ok(compile(Array.from({length: 70}, (_, k) => `plot(${k}, 0, 1);`).join('\n')).count > 64);
console.log('test_kir: PASS');
