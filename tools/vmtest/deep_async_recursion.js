// D38 (docs/vm-L2-design.md sec.10.3): what ends an async function's
// synchronous recursion. `dive` awaits its own call, so every level is a
// first synchronous stretch nested inside the previous one. On the flat
// variants that stretch is a flat frame -- no C frame and no segment byte per
// level -- so the D10 budget never sees it (budget_probe.sh requires
// budget_hits=0) and the descent ends only when the guest heap does. What
// that looks like was MEASURED (host, --profile device, 2026-09-13), and it
// is not the tidy InternalError the design predicted: the descent stops
// around level 77 with the whole chain alive, the failing allocation is too
// small to leave room for an InternalError object (JS_ThrowError2 then
// throws JS_NULL, the case seg_oom_boundary.js's `e === null` already
// knows), and while the chain unwinds some levels' own `await` cannot
// register its reaction either, so their promises end unhandled (vmrun exit
// 2) and the reason that reaches the outer catch is null. Nothing reaches
// the synchronous try. On -recur the upstream C chain per level meets
// async_func_resume's C-stack test first: RangeError at ~15 levels, exit 0.
// budget_probe.sh holds each build to its own measured expected file
// (expected/deep_async_recursion.txt / -recur.txt), diffs only the lines
// below, and reports the unhandled count and the depth on its info line.
//
// The outer catch lives in an async function on purpose: it runs from the
// job drain, after the levels have been released. Anything the top level
// does after the descent -- even `p.catch(...)` -- runs with the heap still
// full and fails the same way the descent did.
//
// Not in corpus/: the right answer differs by build, and under
// --profile device this is a creeping OOM, the shape in which upstream's
// build_backtrace use-after-free shows (known/oom_backtrace_uaf.js), so the
// asan run is recorded, not required (budget_probe.sh).
let depth = 0, syncKind = "none";
async function dive() { depth++; await dive(); }
async function main() {
  try { await dive(); print("caught", "resolved"); }
  catch (e) { print("caught", e === null ? "null" : e.constructor.name); }
}
try { main(); } catch (e) { syncKind = e === null ? "null" : e.constructor.name; }
print("sync-try", syncKind);
print("#info max_depth=" + depth);
