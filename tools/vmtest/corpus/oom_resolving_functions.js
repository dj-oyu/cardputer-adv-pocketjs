// vmrun-flags: --fail-alloc 1351
// vmrun-rom-flags: --fail-alloc 913
// vmrun-keepsrc-flags: --fail-alloc 1352
// 1352 -> 1351 with CONFIG_POCKET_VM_STRIP_FN_SOURCE: 1 function source copy fewer before the target (allocator traces aligned, docs/vm/vm-L2-results.md sec.6).
// Regression for the resolving-functions double-free (reports/upstream/
// quickjs-ng-resolving-functions-double-free.md). js_create_resolving_
// functions() used to leave a dangling value in resolving_funcs[0] when the
// SECOND resolve function's allocation failed, and js_async_function_call's
// fail: path frees both slots unconditionally -- so an allocation failure at
// exactly the wrong point turned into an ASan heap-use-after-free instead of
// a clean thrown error.
//
// f.apply() is deliberate, not incidental: js_vm_flat_callable() only
// special-cases a DIRECT bytecode call (OP_call/OP_call_method) to an async
// function. Every call that reaches JS_Call() from native glue --
// Function.prototype.apply/call/bind, Reflect.apply, a Proxy trap, a
// builtin's callback -- goes through js_async_function_call regardless of
// CONFIG_POCKET_VM_FLATCALLS, so this bug is reachable in the SHIPPED
// default build (flatcalls on) at recursion depth 1, not only through the
// deep synchronous recursion that flat calls turn into heap exhaustion
// (docs/vm-L2-design.md sec.10.3 D38, see budget_probe.sh's
// deep_async_recursion). A plain `f()` here would go flat and miss the
// js_async_function_call path entirely on the shipped config.
//
// --fail-alloc 1352 targets the js_malloc(sizeof(JSPromiseFunctionData))
// call for the SECOND (reject) resolving function inside
// js_create_resolving_functions, reached from JS_NewPromiseCapability inside
// js_async_function_call. Like every --fail-alloc corpus case, this attempt
// number is a snapshot of the current allocation sequence up to that call:
// it depends on how many allocations context bootstrap performs before the
// script runs, which shifts if bootstrap grows or shrinks. If this file
// starts passing trivially (no exception, output changes) or with a
// mismatched failure site, the number needs to be relocated by instrumenting
// js_create_resolving_functions's attempt count before it -- do not just move
// the number until something fails.
//
// Relocated 1353 -> 1352 with backlog #9 (guest usable size = tlsf block
// length: the allocation sequence before it is one attempt shorter), by the
// instrumentation described above. The -alloca builds number it 1351 (and
// numbered it 1352 before the relocation too), so there this case exercises
// a neighbouring allocation, as it already did.
//
// Blessed on asan-recur AND checked to match under the default (flatcalls)
// variant: the whole point of the fix is that the failure is caught the same
// way, at the same JS-observable point, on both the path that keeps the C
// recursion for a call and the path that flattens it (this call never
// flattens either way, see above).
async function f() {}
try {
  f.apply(null, []);
  print("no-throw");
} catch (e) {
  print("caught", e instanceof Error, e.constructor.name);
}
print("done");
