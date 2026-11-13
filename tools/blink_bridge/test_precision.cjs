"use strict";
// Pure numeric examples: no model payload or claim of universal driver semantics.
const assert = require("node:assert/strict");
const cases = [
  [0.979999, false, false],
  [0.97999999, false, true],
  [0.98, true, true],
  [0.98000001, true, true],
];
for (const [x, binary64, binary32] of cases) {
  assert.equal(x >= 0.98, binary64);
  assert.equal(Math.fround(x) >= Math.fround(0.98), binary32);
}
assert.notEqual(Math.fround(0.98), 0.98);
console.log("PASS: 4 precision boundary cases; binary64 and binary32 are distinct contracts");
