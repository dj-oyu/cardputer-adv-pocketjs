// plan_js.mjs: the subset's errors name the line and the rule, and what it
// accepts compiles and runs (host, Node):  node tools/kasane_ir/test_plan_js.mjs
import assert from 'node:assert/strict';
import {findPlans, compilePlan, reference} from './plan_js.mjs';

const plan = (body, head = 'inputs: a, b', params = '') =>
  findPlans(`// file\n/** @plan t ${head} */\nfunction t(${params}) {\n${body}\n}\n`, 'test.js')[0];
const rejects = (body, rule, line = 4) => assert.throws(() => compilePlan(plan(body)),
  e => e.message.includes(`line ${line}:`) && e.message.includes(`[${rule}:`), `${body} -> ${rule}`);

rejects('while (a > b) { move(a, b); }', 'R3');
rejects('for (let i = 0; i < 3; i++) { continue; }', 'R3');
rejects('for (let i = 1; i < 3; i++) { move(a, b); }', 'R3');
rejects('let n = 3;\nfor (let i = 0; i < n; i++) { n += 1; }', 'R3', 5);
rejects('for (let i = 0; i < 3; i++) { i += 1; }', 'R4');
rejects('for (let i = 0; i < 3; i++) { if (a > b) break; else move(a, b); }', 'R6');
rejects('if (a >= b) break;', 'R6');
rejects('if (a > b) break;', 'R6');
rejects('const x = [a, b];', 'R5');
rejects("const x = 'a';", 'R5');
rejects('const x = a % 2;', 'R5');
rejects('const x = Math.cos(a);', 'R2');
rejects('const x = foo(a);', 'R5');
rejects('const x = a < b;', 'R5');
rejects('const a = 1;', 'R4');
rejects('const x = 1;\nx = 2;', 'R4', 5);
rejects('move(c, a);', 'R4');
rejects('let x = 0;\nx++;', 'R1', 5);
rejects('print(a);', 'R5');
rejects('for (let i = 0; i < 2; i++) { const x = a; }\nmove(x, b);', 'R4', 5);
rejects('move(a);', 'R1');

// Accepted: Math.sin, Math.PI, a counter read in the body, braced break,
// a parameter as colour and count, a constant 0 count (no loop).
const ok = plan(`let x = a;
for (let i = 0; i < n; i++) {
  if (x > b) { break; }
  plot(x + i * 2, Math.sin(Math.PI * x), c);
  x += 1;
}
for (let k = 0; k < 0; k++) { move(a, a); }`, 'inputs: a, b', 'n, c');
const c = compilePlan(ok);
assert.ok(c.count > 0 && c.count <= 64);
const r = reference(ok, [3, 7, 0, 0, 0, 0, 0, 0], [6, 0xffff]);
assert.equal(r.status, 'DONE');
assert.equal(r.seg.length, 5);          // x = 3..7, then x > b breaks
assert.deepEqual(r.seg[1].slice(0, 2), [6, 0]);
// An empty plan is one SET (register() needs an instruction).
assert.equal(compilePlan(plan('', 'inputs:')).text, 'S0,0');
console.log('test_plan_js: PASS');
