// vmrun-flags: --profile device
// Regression for docs/vm/backlog.md #5. quickjs-ng starts malloc_gc_threshold
// at 256 KiB, above the guest's 160 KiB limit; until guest.c/vmrun.c lowered
// it to half the limit, no cycle collection ever ran and this loop died of
// OOM after a few hundred cycles (host) with "cycles-exhaust-heap true".
// Every iteration leaves one unreachable two-object cycle; 100000 of them are
// far more than 160 KiB, so finishing means the collector ran, repeatedly.
let n = 0;
let caught = false;
try {
  for (; n < 100000; n++) {
    const a = { n }, b = { a };
    a.b = b;
  }
} catch (e) { caught = true; }
print("cycles-exhaust-heap", caught);
print("cycles", n);
