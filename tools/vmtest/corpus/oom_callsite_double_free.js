// vmrun-flags: --fail-alloc 1455
// vmrun-skip-variants: asan-alloca o2-alloca asan-alloca-keepsrc o2-alloca-keepsrc -- no frame segment on these builds, so the target allocation is attempt 1456 and 1458 misses it
// vmrun-keepsrc-flags: --fail-alloc 1458
// 1458 -> 1455 with CONFIG_POCKET_VM_STRIP_FN_SOURCE: 3 function source copies fewer before the target (allocator traces aligned, docs/vm/vm-L2-results.md sec.6). The numbers below are the -keepsrc ones.
// Regression for upstream quickjs-ng c846cb1364 ("Fix double free of a
// CallSite when the backtrace array insertion fails"), backported with
// docs/vm/backlog.md #6. With Error.prepareStackTrace set, build_backtrace
// builds an array of CallSite objects. When inserting one into that array
// failed, JS_DefinePropertyValueUint32 had already freed the CallSite and the
// caller freed it again (ASan: heap-use-after-free in JS_FreeValue from
// build_backtrace). js_new_callsite also took the csd values without clearing
// them, so the cleanup loop after the break freed those a second time.
//
// --fail-alloc 1458 targets the fast-array growth for index 0 inside that
// insertion (found by instrumenting both failure branches and sweeping
// --fail-alloc; 1462 is the same insertion for index 1). Heap-margin sweeps
// under the device limit were tried first (up to 1,200 runs each) and never
// reached this branch: either the CallSite allocation failed or everything
// fitted. Like every --fail-alloc case, the
// number is a snapshot of the allocation sequence; if the output stops
// matching, re-find it the same way rather than moving it until it fails.
Error.prepareStackTrace = (e, frames) => frames;
function mk(d) { return d > 0 ? mk(d - 1) : new Error("x"); }
let r;
try { r = mk(6); print("returned", typeof r); } catch (e) { print("threw", e === null ? "null" : e.constructor.name); }
Error.prepareStackTrace = undefined;
print("after", [1, 2, 3].map((x) => x * 2).join());
