// G1 for the async path (docs/vm-L2-design.md sec.12.2, D34): the same
// measurement as stack_probe.js with the recursing function declared async.
// No await anywhere, so every level is the first synchronous stretch of an
// async call made from JS -- the stretch L2b-async runs as a flat frame in
// the caller's C activation. On the flat variants the C stack must not grow
// per level (0 bytes: NOT_PROPORTIONAL); on -recur each level is the upstream
// C chain (JS_CallInternal -> js_async_function_call -> async_func_resume ->
// JS_CallInternal) and stays PROPORTIONAL. Nothing bounds the depth but the
// heap (D38: these frames are not in a segment and the byte budget does not
// see them), so the depths stack_probe.sh uses (2000 / 4000) must stay well
// inside the host profile's 64 MiB, which they do.
//
//   tools/vmtest/stack_probe.sh 2000 o2 stack_probe_async.js
if (typeof __VMTEST_PROBE_DEPTH === "undefined") {
  throw new Error("stack_probe_async.js requires --include <depth-snippet> defining __VMTEST_PROBE_DEPTH");
}

async function dive(n) {
  // Probe BEFORE recursing, exactly as stack_probe.js does; the unawaited
  // promise of the inner call is dropped -- there is nothing to await, the
  // whole descent is synchronous.
  __vmtest_stack_probe();
  if (n > 0) dive(n - 1);
}
dive(__VMTEST_PROBE_DEPTH);
print("dive depth=" + __VMTEST_PROBE_DEPTH + " done");
