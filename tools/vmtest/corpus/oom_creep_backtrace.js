// vmrun-flags: --profile device
// Regression for docs/vm/backlog.md #6 (upstream quickjs-ng e1c1e416 / #1469):
// an OOM that creeps up on the limit, so the exception's backtrace is built
// with almost no heap left. build_backtrace held the thrown error only by a
// borrowed reference; an allocation failing inside it replaced (and freed)
// rt->current_exception and the rest of the function read the freed object.
// ASan builds reported heap-use-after-free in can_add_backtrace.
//
// This keeps the path memory_device.js's array-oom used to take (it grew by
// 1.5x and crept up on the limit; after backlog #9 it reached this UAF on
// asan and was replaced there by a single large request). What is asserted
// is only what must hold whatever the margin: every failure is caught and the
// runtime works afterwards. How many caught values are a bare null
// (JS_ThrowOutOfMemory's own allocation also failing) depends on bytes left
// and is recorded as #info, not diffed.
// The bad window is narrow: enough heap left for the Error object, not
// enough for its backtrace. A single run lands in it only by chance (a
// one-byte change to known/oom_backtrace_uaf.js can stop it reproducing), so
// this sweeps the margin: fill the heap with small objects to the limit,
// release r of them, then make one large request from a few frames down, for
// r = 0..47 -- the margin grows by one small object per step.
function big(depth) {
  if (depth > 0) return big(depth - 1);
  return "x".repeat(1 << 20);
}
let caught = 0, nulls = 0;
for (let r = 0; r < 48; r++) {
  let hog = [];
  try { for (;;) hog.push({ n: hog.length }); } catch (e) {}
  hog.length = hog.length > r ? hog.length - r : 0;
  try { big(3); } catch (e) { caught++; if (e === null) nulls++; }
  hog = null;
}
print("margin-sweep-caught", caught);
print("#info margin_sweep_null=" + nulls);

const after = [];
for (let i = 0; i < 100; i++) after.push({ i });
print("after-oom", after.length, JSON.stringify(after[99]));
