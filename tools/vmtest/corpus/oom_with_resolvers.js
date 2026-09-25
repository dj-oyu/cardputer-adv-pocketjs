// vmrun-flags: --fail-alloc 1369
// vmrun-rom-flags: --fail-alloc 931
// vmrun-skip-variants: asan-alloca o2-alloca asan-alloca-keepsrc o2-alloca-keepsrc -- no frame segment on these builds, so the allocation numbers differ
// vmrun-keepsrc-flags: --fail-alloc 1370
// 1370 -> 1369 with CONFIG_POCKET_VM_STRIP_FN_SOURCE: 1 function source copy fewer before the target (allocator traces aligned, docs/vm/vm-L2-results.md sec.6). The numbers below are the -keepsrc ones.
// Regression for upstream quickjs-ng 49131a6315 ("Fix reference count bug in
// Promise.withResolvers"). JS_DefinePropertyValue consumes its value even when
// it fails, but js_promise_withResolvers only cleared its local after a
// SUCCESSFUL define, so an OOM inside one of the three defines freed that
// value again on the exception path (ASan: heap-use-after-free in JS_FreeValue
// from js_promise_withResolvers, freeing resolving_funcs[1] after the reject
// define failed). --fail-alloc 1370 was found by sweeping 1250-1400 with an
// ASan build of the unfixed function (1370 and 1371 hit); it is a snapshot of
// the allocation sequence and has to be re-found the same way if it moves.
let r = null;
try { r = Promise.withResolvers(); } catch (e) {}
print("with-resolvers", r === null ? "threw" : typeof r.resolve);
print("after", [1, 2, 3].map((x) => x * 2).join());
