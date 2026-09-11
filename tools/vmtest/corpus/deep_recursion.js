// Recursion under the host stack (--profile host: 7 MiB stack limit).
// The depth reached is printed as "#info" (not diffed): it is a property of
// the C stack per JS frame, which L2 is meant to change. What is diffed is
// that overflow is a catchable RangeError and that the VM recovers.
function sum(n) { return n === 0 ? 0 : n + sum(n - 1); }
print("sum(1000)", sum(1000));

function even(n) { return n === 0 ? true : odd(n - 1); }
function odd(n) { return n === 0 ? false : even(n - 1); }
print("mutual", even(2000), odd(2001));

// Recursion through a try/finally at every level: unwinding runs 500 finallys.
let unwound = 0;
function deepThrow(n) {
  try {
    if (n === 0) throw new Error("bottom");
    return deepThrow(n - 1);
  } finally { unwound++; }
}
try { deepThrow(500); } catch (e) { print("unwound", unwound, e.message); }

let depth = 0;
function dive() { depth++; dive(); }
let kind = "none";
try { dive(); } catch (e) { kind = e.constructor.name + ": " + e.message; }
print("overflow", kind);
print("#info max_depth=" + depth);
print("depth>1000", depth > 1000);

// The VM must be usable after an overflow.
print("after", sum(100));
let d2 = 0;
function dive2(x) { d2++; return dive2(x + 1) + 1; }
try { dive2(0); } catch (e) { print("again", e instanceof RangeError); }
print("again-deep", d2 > 1000);
