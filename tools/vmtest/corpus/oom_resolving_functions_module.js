// vmrun-flags: --module --fail-alloc 1290
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
// --fail-alloc 1290 targets the js_malloc(sizeof(JSPromiseFunctionData)) call
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
// cleanly afterward. Blessed on asan-recur and checked to match under the
// default (flatcalls) variant, matching oom_resolving_functions.js's
// convention: this call site does not distinguish flat vs. recursive calls
// (JS_NewPromiseCapability's own C recursion does not depend on
// CONFIG_POCKET_VM_FLATCALLS), so all six variants are expected to agree.
export const x = 1;
