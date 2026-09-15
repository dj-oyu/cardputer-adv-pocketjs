# quickjs-ng: resolving-functions allocation failure leaves a dangling value, double-freed by `js_async_function_call`

Status: **not sent upstream**. Filed locally for reference; the fix below is
applied in this fork (`vm/l2c-oomfix`) but no issue/PR has been opened against
quickjs-ng.

## Affected version

quickjs-ng **0.14.0**, vendored in this repository at
`components/quickjs-ng/quickjs-ng/quickjs.c` via Espressif's
`esp-iot-solution` component wrapper (`idf_component.yml`:
`repository_info.commit_sha: 0d0b99456ce9112f4ac4838927ba837949359f9b`,
`version: 0.14.0`), plus this repo's local "immutable-buffer" patch
(`6de51f4`, unrelated to this bug -- it does not touch promise or async
machinery). The functions named below (`js_create_resolving_functions`,
`js_async_function_call`, `js_async_function_free0`,
`js_async_function_resolve_create`) are upstream quickjs-ng functions; this
report was written against the vendored copy but the defect and the fix are
in unmodified upstream code, and the same bug is present in current
quickjs-ng `main` as of this writing (verified by reading, not by running
upstream's own test suite -- see "Reproducing upstream" below for why a host
run needs a stand-in for this repo's `--fail-alloc`).

## Symptom

An allocation failure while building the pair of resolve/reject functions for
a promise capability can leave a stale (already-freed) `JSValue` in the
caller's output slot. The most common caller-side consequence, seen here, is
an immediate ASan `heap-use-after-free` (or, without ASan, a use of freed
heap and likely later heap corruption) inside `js_async_function_call`'s
`fail:` path, reached from calling **any** async function through
`Function.prototype.apply`/`.call`/`Function.prototype.bind`, `Reflect.apply`,
a `Proxy` trap, or any other C-level `JS_Call()` into an async function --
not only through the deep C recursion the comments in this codebase's own
history assumed was the only way to reach it (see "Reachability" below).

## Root cause

`js_create_resolving_functions()` builds two function objects in a loop
(`i = 0, 1`: resolve, then reject) and writes each into
`resolving_funcs[i]` only after both of that iteration's allocations
(`JS_NewObjectProtoClass` and `js_malloc(sizeof(JSPromiseFunctionData))`)
succeed:

```c
static int js_create_resolving_functions(JSContext *ctx,
                                         JSValue *resolving_funcs,
                                         JSValueConst promise)
{
    JSValue obj;
    JSPromiseFunctionData *s;
    JSPromiseFunctionDataResolved *sr;
    int i, ret;

    sr = js_malloc(ctx, sizeof(*sr));
    if (!sr) {
        return -1;
    }
    sr->ref_count = 1;
    sr->already_resolved = false;
    ret = 0;
    for (i = 0; i < 2; i++) {
        obj = JS_NewObjectProtoClass(ctx, ctx->function_proto,
                                     JS_CLASS_PROMISE_RESOLVE_FUNCTION + i);
        if (JS_IsException(obj)) {
            goto fail;
        }
        s = js_malloc(ctx, sizeof(*s));
        if (!s) {
            JS_FreeValue(ctx, obj);
fail:
            if (i != 0) {
                JS_FreeValue(ctx, resolving_funcs[0]);
            }
            ret = -1;
            break;
        }
        sr->ref_count++;
        s->presolved = sr;
        s->promise = js_dup(promise);
        JS_SetOpaqueInternal(obj, s);
        js_function_set_properties(ctx, obj, JS_ATOM_empty_string, 1);
        resolving_funcs[i] = obj;
    }
    js_promise_resolve_function_free_resolved(ctx->rt, sr);
    return ret;
}
```

When the **first** iteration (`i == 0`) fails, `resolving_funcs[0]` is never
written by this function at all -- it is left holding whatever the caller put
there before the call. Every caller in this codebase pre-initializes both
slots to `JS_UNDEFINED`, so this path is safe in practice (though only by
caller convention, not by contract).

When the **second** iteration (`i == 1`) fails, `resolving_funcs[0]` already
holds the live object from the first iteration. The `fail:` block frees it
(`JS_FreeValue(ctx, resolving_funcs[0])`) -- correctly, since this function
owns the reference until it hands it to the caller on success -- but then
**does not clear `resolving_funcs[0]`**. The function returns `-1`, and the
caller receives a `resolving_funcs[0]` that looks exactly like a valid
`JS_TAG_OBJECT` value but points at freed heap memory.

The contract every caller *should* be able to rely on -- "on failure,
`resolving_funcs[0]` and `[1]` are either live or `JS_UNDEFINED`, never a
value this function has already freed" -- silently does not hold for the
`i == 1` failure path.

### Where it bites

`js_async_function_call` (the class `.call` for `JS_CLASS_ASYNC_FUNCTION`):

```c
static JSValue js_async_function_call(JSContext *ctx, JSValueConst func_obj,
                                      JSValueConst this_obj,
                                      int argc, JSValueConst *argv, int flags)
{
    JSValue promise;
    JSAsyncFunctionData *s;

    s = js_mallocz(ctx, sizeof(*s));
    if (!s) {
        return JS_EXCEPTION;
    }
    s->header.ref_count = 1;
    add_gc_object(ctx->rt, &s->header, JS_GC_OBJ_TYPE_ASYNC_FUNCTION);
    s->is_active = false;
    s->resolving_funcs[0] = JS_UNDEFINED;
    s->resolving_funcs[1] = JS_UNDEFINED;

    promise = JS_NewPromiseCapability(ctx, s->resolving_funcs);
    if (JS_IsException(promise)) {
        goto fail;
    }

    if (async_func_init(ctx, &s->func_state, func_obj, this_obj, argc, argv)) {
fail:
        JS_FreeValue(ctx, promise);
        js_async_function_free(ctx->rt, s);
        return JS_EXCEPTION;
    }
    ...
```

`js_async_function_free` -> `js_async_function_free0` frees both
`s->resolving_funcs[0]` and `[1]` unconditionally:

```c
static void js_async_function_free0(JSRuntime *rt, JSAsyncFunctionData *s)
{
    js_async_function_terminate(rt, s);
    JS_FreeValueRT(rt, s->resolving_funcs[0]);
    JS_FreeValueRT(rt, s->resolving_funcs[1]);
    remove_gc_object(&s->header);
    js_free_rt(rt, s);
}
```

If `JS_NewPromiseCapability` -> `js_create_resolving_functions` failed on the
**second** resolving function, `s->resolving_funcs[0]` is the dangling value
described above, and `js_async_function_free0`'s unconditional free double-frees
it: an ASan `heap-use-after-free` (`JS_FreeValueRT` reading the freed
object's ref-count field), or plain heap corruption without ASan.

The same shape of bug exists in **`js_async_function_resolve_create`** (used
by `js_async_function_settle_core` when an async function awaits a value),
which has the identical `if (i == 1) JS_FreeValue(ctx, resolving_funcs[0]);`
without clearing the slot. Its one caller does not currently free that local
array on the failure path, so it is not exploitable today, but it relies on
the same fragile convention and should be fixed for the same reason (see
"Fix" below; not changed in this patch because no live caller depends on it,
but flagged here so it does not bite a future caller).

## Reachability

The comment this fork previously carried (see `git log -p` on
`js_async_function_call`'s call site in `main/quickjs-ng`'s vendored copy,
stage A2) assumed this bug was reachable **only** through a deep, C-recursive
async call chain -- i.e. that `js_async_function_call` itself is normally
guarded by quickjs's C-stack-depth check, which throws a `RangeError` (stack
overflow) long before an allocation failure can occur, so upstream never
sees this in practice.

That assumption is only half true, and this repo's own architecture makes it
concretely false. This fork's `CONFIG_POCKET_VM_FLATCALLS` path
(`js_vm_flat_callable()`) special-cases a **direct bytecode call**
(`OP_call`/`OP_call_method`) to an async function to skip the C recursion
entirely -- but every call that reaches the async function through
`JS_Call()` from *native* glue code still goes through the ordinary,
C-recursive `js_async_function_call`, regardless of the flat-calls
configuration:

- `Function.prototype.apply` / `.call`
- a function produced by `Function.prototype.bind`
- `Reflect.apply`
- a `Proxy` trap
- any built-in that invokes a user callback via `JS_Call` (`Array.prototype.map`,
  `Promise.prototype.then`'s reaction job, etc.)

So on **this fork's shipped default configuration** (flat calls on),
`asyncFn.apply(null, [])` reaches `js_async_function_call` at recursion depth
1 -- no deep recursion needed. Confirmed on this fork's host harness
(`tools/vmtest/vmrun.c --fail-alloc N`, ASan build, default variant i.e.
`CONFIG_POCKET_VM_FLATCALLS=1`):

```js
async function f() {}
f.apply(null, []);
```

with the Nth heap allocation attempt made to fail (the
`js_malloc(sizeof(JSPromiseFunctionData))` for the *second* resolving
function) reproduces the exact ASan report below, at recursion depth 1.

This matters for upstream too: **any embedder or codebase that calls an
async function through `.apply`/`.call`/`.bind`/`Reflect.apply`/a `Proxy`, or
lets a built-in invoke one as a callback, can hit this at any call depth, on
unmodified quickjs-ng, given an allocation failure at the right moment.**
Depth is not what gates this bug in vanilla quickjs-ng either; it is whether
the call reaches `js_async_function_call` through `JS_CallInternal`'s bytecode
`OP_call` fast path (upstream does not itself distinguish "flat" and
"recursive" calls, but as of this writing has no fast path around
`js_async_function_call` for a *direct* call either, so on stock quickjs-ng
every async call -- direct or wrapped -- goes through
`js_async_function_call`, and the C-stack-depth guard on `JS_CallInternal`'s
entry is what makes a stray allocation failure here rare, not depth). An
embedder with `JS_SetMemoryLimit` set tightly relative to its stack limit, or
one that runs with a very deep permitted stack, can plausibly reach the
narrow allocation window with only a handful of nested async calls, not
thousands.

## Minimal reproduction on this fork's host harness

```js
async function f() {}
f.apply(null, []);
```

Run with `tools/vmtest/vmrun.c --profile host --fail-alloc 1319` (default,
i.e. flatcalls-on, ASan build; the specific attempt number is a snapshot of
this fork's runtime-bootstrap allocation count and has no meaning upstream --
see "Reproducing upstream" for how to find the equivalent point without this
harness's `--fail-alloc`).

ASan output (captured on this fork's `vmrun-asan` before the fix; abridged to
the frames that matter):

```
==52262==ERROR: AddressSanitizer: heap-use-after-free on address 0x50b000008930 at pc 0x642402a6c2e9 bp 0x7ffff7d0d400 sp 0x7ffff7d0d3f0
READ of size 4 at 0x50b000008930 thread T0
    #0 JS_FreeValueRT quickjs.c:7254
    #1 js_async_function_free0 quickjs.c:22178
    #2 js_async_function_free quickjs.c:22187
    #3 js_async_function_call quickjs.c:22400
    #4 JS_CallInternal quickjs.c:18589
    #5 JS_Call quickjs.c:21700
    #6 js_function_apply quickjs.c:43774
    #7 js_call_c_function quickjs.c:18289
    #8 JS_CallInternal quickjs.c:18589
    ...

freed by thread T0 here:
    #0 free (asan interceptor)
    #1 js_free_rt quickjs.c:1744
    #2 free_object quickjs.c:7151
    #3 free_gc_object quickjs.c:7159
    #4 free_zero_refcount quickjs.c:7182
    #5 js_free_value_rt quickjs.c:7226
    #6 JS_FreeValueRT quickjs.c:7255
    #7 JS_FreeValue quickjs.c:7262
    #8 js_create_resolving_functions quickjs.c:57038   <- the un-cleared free
    #9 js_promise_new quickjs.c:57198
    #10 js_new_promise_capability quickjs.c:57286
    #11 JS_NewPromiseCapability quickjs.c:57315
    #12 js_async_function_call quickjs.c:22392

previously allocated by thread T0 here:
    #0 malloc (asan interceptor)
    #1 js_malloc_rt quickjs.c:1718
    #2 js_malloc quickjs.c:1811
    #3 JS_NewObjectFromShape quickjs.c:6071
    #4 JS_NewObjectProtoClass quickjs.c:6200
    #5 js_create_resolving_functions quickjs.c:57024   <- the first (i=0) object
    #6 js_promise_new quickjs.c:57198
    #7 js_new_promise_capability quickjs.c:57286
    #8 JS_NewPromiseCapability quickjs.c:57315
    #9 js_async_function_call quickjs.c:22392
```

The same report, with the same three frames
(`js_async_function_call` -> `js_async_function_free` -> `js_async_function_free0`
-> `JS_FreeValueRT`) at the top, was also produced with the plain, direct
call `f()` (no `.apply`) when built with `CONFIG_POCKET_VM_FLATCALLS`
disabled (this fork's "recur" build variant, closer to upstream's control
flow, at `--fail-alloc 1308` for that build's allocation sequence), and by
failing any of the *earlier* sub-allocations inside the second
`JS_NewObjectProtoClass` call (shape/property-table allocations), not only
the `JSPromiseFunctionData` malloc -- every allocation-failure point on the
`i == 1` branch reaches the same use-after-free, because they all take the
same `fail:` label.

## Reproducing upstream (unmodified quickjs-ng, no `--fail-alloc`)

quickjs-ng's C API has no direct allocation-attempt-count injection. Two
practical ways to hit the same window on stock quickjs-ng:

1. **`JS_SetMemoryLimit`** to a byte count computed (by trial, bisecting
   the limit) to leave just enough headroom for the first resolving
   function's object + `JSPromiseFunctionData` but not the second's. This is
   exactly what this fork's `--fail-alloc` does deterministically; upstream
   would need a bisection loop over the memory limit, re-running the
   reproduction script at each limit, since the accounted size is the
   allocator's reported usable size, not the raw allocation count -- more
   fiddly than `--fail-alloc N` but the same effect.
2. **A custom `JSMallocFunctions`** (`JS_NewRuntime2`) that counts calls and
   returns `NULL` at a chosen count -- functionally identical to this fork's
   `vmrun.c` allocator, and the most direct port of the reproduction above:
   count allocation attempts, fail the one that lands inside the second
   resolving function's construction, run `f.apply(null, [])` (or plain
   `f()` -- both reach `js_create_resolving_functions` the same way; only
   `.apply` guarantees reaching it through `js_async_function_call` even on
   a runtime with a flat-call fast path like this fork's, which upstream
   does not have and therefore does not need).

Either way, the observable symptom on unmodified quickjs-ng (built with
`-fsanitize=address`) should be the same `heap-use-after-free` reported from
inside `js_async_function_free0`, with `js_create_resolving_functions` named
as the freeing site in the "freed by" stack.

## Fix

Make `js_create_resolving_functions` honor the contract every caller already
assumes: never leave a value it has already freed in an output slot. The
`fail:` path is the only place that can violate it (see analysis above), so
clear the slot it just freed:

```c
            if (i != 0) {
                JS_FreeValue(ctx, resolving_funcs[0]);
                resolving_funcs[0] = JS_UNDEFINED;
            }
```

This is sufficient: `resolving_funcs[i]` is never written by this function
before its allocations for iteration `i` have succeeded (the assignment is
the last statement in the loop body), so the only slot this function can
poison is `resolving_funcs[0]` on an `i == 1` failure, and clearing it there
closes the gap for every caller -- `js_async_function_call`,
`js_promise_constructor` (via `js_promise_new` /
`js_new_promise_capability`), `js_promise_resolve_thenable_job`, and every
`JS_NewPromiseCapability` caller in the file (`js_promise_resolve`,
`js_promise_withResolvers`, `js_async_generator_next`, `JS_LoadModule`,
`js_dynamic_import`, `js_inner_module_evaluation`'s cycle-root promise via
`js_evaluate_module`) -- without needing to audit or change any of them.
Verified by reading each: every caller other than `js_async_function_call`
either (a) checks `JS_IsException(promise)`/the `js_create_resolving_functions`
return value and returns/branches away without touching `resolving_funcs` on
failure, so it was never affected by the stale value in the first place, or
(b) is a persistent struct field (`JSModuleDef.resolving_funcs`) freed
unconditionally by its own finalizer later, in which case this fix closes a
second, previously unnoticed instance of the exact same double-free (see
below).

### A second instance the fix also closes: module evaluation

`js_evaluate_module`'s cycle-root path
(`m->promise = JS_NewPromiseCapability(ctx, m->resolving_funcs)`) stores the
resolving functions directly into the persistent `JSModuleDef`. Its module
finalizer frees `m->resolving_funcs[0]`/`[1]` unconditionally, the same
pattern as `js_async_function_free0`. Before this fix, an allocation failure
on the *second* resolving function during a cycle-root module's evaluation
would leave the same dangling value in `m->resolving_funcs[0]`, later
double-freed when the module is finalized. This was not separately
instrumented or reproduced (module evaluation's allocation sequence was not
mapped the way the async-call one was), but it is the same root cause and
the same fix closes it; noted here so a future reader does not have to
re-derive it.

### `js_async_function_resolve_create`: same shape, not fixed here

As noted above, `js_async_function_resolve_create` has the identical
`if (i == 1) JS_FreeValue(ctx, resolving_funcs[0]);` without clearing the
slot, for the resolve/reject pair created for an async function's `await`.
Its one call site (`js_async_function_settle_core`) does not free the local
`resolving_funcs[2]` array on the failure path (`goto fail` bypasses the
`JS_FreeValue(ctx, resolving_funcs[i])` loop that runs only on success), so
this is not exploitable through any path found in this repo today. It is the
same defect, though, and worth the same one-line fix if this is ever taken
upstream, so a later change to `js_async_function_settle_core` (or a new
caller) does not reintroduce this bug's twin. Not included in the patch
attached here, to keep the patch minimal and scoped to a confirmed,
reproduced defect.

## What changed in this fork as a result

- `js_create_resolving_functions`'s `fail:` path now clears
  `resolving_funcs[0]` after freeing it (the root fix; see attached patch).
- `flat_async_call`'s `if (JS_IsException(promise))` handler in this fork's
  `main/quickjs-ng` copy (stage A2, commit history on `vm/l2c`) previously
  worked around this same bug locally, by resetting
  `s->resolving_funcs[0]`/`[1]` to `JS_UNDEFINED` itself before calling
  `js_async_function_free`. That local workaround is now redundant --
  `js_async_function_free`'s unconditional free is safe by construction once
  the root cause is fixed -- and has been removed in favor of the shared fix,
  with a comment pointing at `js_create_resolving_functions`'s contract.
- A regression case, `tools/vmtest/corpus/oom_resolving_functions.js`, drives
  `f.apply(null, [])` with `--fail-alloc` pointed at the second resolving
  function's data allocation. Blessed against the `asan-recur` build
  (upstream's own C-recursive call shape) and confirmed byte-identical across
  every other build variant this repo's `tools/vmtest/run.sh` exercises,
  including the default flat-calls build.

## Patch

See `quickjs-ng-resolving-functions-double-free.patch` in this directory
(`git format-patch` output against this fork's vendored copy of quickjs-ng
0.14.0, commit `8511f6b` on branch `vm/l2c-oomfix`).

## Disclosure

Not filed upstream. This is a local report only, kept for this repository's
own record and to accompany the fork's local fix; the patch here is scoped
to what this fork could reproduce and verify, not written as an upstream PR.
