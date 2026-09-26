// vmrun-flags: --module --fail-alloc 1289
// vmrun-rom-keepsrc-flags: --module --fail-alloc 849
// vmrun-rom-lb-flags: --module --fail-alloc 763
// vmrun-rom-lb-keepsrc-flags: --module --fail-alloc 763
// vmrun-lb-flags: --module --fail-alloc 1013
// vmrun-rom-flags: --module --fail-alloc 849
// vmrun-skip-variants: asan-alloca o2-alloca asan-alloca-keepsrc o2-alloca-keepsrc -- no frame segment on these builds, so the target allocation is attempt 1288 and 1289 misses it (swept, docs/vm-L2-design.md sec.10.3)
// Regression for the SECOND instance of the resolving-functions double-free
// (reports/upstream/quickjs-ng-resolving-functions-double-free.md, "A second
// instance the fix also closes: module evaluation"). js_evaluate_module's
// cycle-root path stores resolving functions directly into the persistent
// JSModuleDef (m->resolving_funcs), which js_free_module_def frees
// unconditionally -- the same unconditional-free shape as
// js_async_function_free0 in oom_resolving_functions.js, but with the module
// finalizer as the second freeing site instead of an immediate double
// JS_FreeValueRT.
//
// --fail-alloc 1289 targets the js_malloc(sizeof(JSPromiseFunctionData)) call
// for the SECOND (reject) resolving function inside
// js_create_resolving_functions, reached from
// JS_NewPromiseCapability(ctx, m->resolving_funcs) in js_evaluate_module for
// this module's own cycle-root promise. Found by instrumenting the vmtest
// allocator's attempt counter (VMTEST_DEBUG_ATTEMPTS=1, temporary, not
// committed) side by side with a one-off print at js_create_resolving_
// functions's loop body; like every --fail-alloc corpus case this number is a
// snapshot of the current bootstrap allocation sequence -- see
// oom_resolving_functions.js's header for what to do if it stops landing on
// the right allocation.
//
// Before the fix (js_create_resolving_functions's fail: path not clearing
// resolving_funcs[0]), this reproduced a heap-use-after-free at
// gc_decref_child (JS_FreeRuntime -> JS_RunGC -> gc_decref -> mark_children
// -> js_mark_module_def -> JS_MarkValue), reading the freed
// JSPromiseFunctionData through the dangling m->resolving_funcs[0] left by
// js_create_resolving_functions's own JS_FreeValue at the fail: label --
// different call site than the async-call instance (that one is a second
// JS_FreeValueRT in js_async_function_free0, this one is a GC mark read
// during teardown) but the same root cause and the same one-line fix.
//
// The module never resolves (evaluation throws before the module body runs),
// so there is nothing to await -- the observable effect is simply the
// eval failing with an out-of-memory error and the runtime tearing down
// cleanly afterward. Blessed on asan-recur; byte-identical on the six
// segment-stack variants (asan, o2, *-recur, *-flat). Not on *-alloca: those
// builds allocate no frame segment, so every allocation after the first call
// is numbered one lower -- the target is attempt 1288 there, and 1289 lands
// on a harmless allocation (exit 0). One number cannot hit the same
// allocation on both, and the shipped path is the segment-stack one, hence
// the skip header above rather than a different number.
// Relocated 1290 -> 1289 with backlog #9 (guest usable size = tlsf block
// length): the allocation sequence before this point is one attempt
// shorter. Re-found by instrumenting the attempt counter at
// js_create_resolving_functions, as above.
export const x = 1;
