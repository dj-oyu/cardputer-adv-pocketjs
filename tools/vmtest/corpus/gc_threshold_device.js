// vmrun-flags: --profile device
// Records a property of the CURRENT device configuration, not a language rule:
// quickjs-ng starts malloc_gc_threshold at 256 KiB (quickjs.c JS_NewRuntime2)
// and nothing in the firmware lowers it, while the guest's limit is 160 KiB.
// The automatic cycle collection therefore never triggers before the limit
// does, and cyclic garbage accumulates until OOM. The trace of this file has
// no "# gc" line. If a level changes this (e.g. sets JS_SetGCThreshold), this
// expected output changes on purpose and must be re-blessed with the reason.
let n = 0;
let caught = false;
try {
  for (; n < 100000; n++) {
    const a = { n }, b = { a };
    a.b = b;
  }
} catch (e) { caught = true; }
print("cycles-exhaust-heap", caught);
print("#info cycles_before_oom=" + n);
