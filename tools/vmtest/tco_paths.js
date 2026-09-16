"use strict";
const N = globalThis.TCO_DEPTH || 100000;
function conditional(n) { return n ? conditional(n - 1) : 42; }
function caught(n) {
  if (!n) return 42;
  try { throw null; } catch (error) { return caught(n - 1); }
}
print("conditional", conditional(N));
print("catch", caught(N));
function logical(n) { return n && logical(n - 1); }
function nullish(n) { return (n ? undefined : 42) ?? nullish(n - 1); }
let effects = 0;
function comma(n) { return (effects++, n ? comma(n - 1) : 42); }
print("logical", logical(N), nullish(N));
print("comma", comma(N), effects);
