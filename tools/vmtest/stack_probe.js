// G1 (docs/vm-L2-design.md sec.1.3): pure JS recursion, C stack per level.
//
// Not part of corpus/ on purpose: this file's OUTPUT (the max depth reached
// by "dive") is meant to change as __VMTEST_PROBE_DEPTH changes, so it cannot
// have one expected/*.txt the way a corpus file does. stack_probe.sh is the
// harness that runs it at two depths and reads the "#info stack_probe" line
// vmrun prints to stderr (excluded from any future diff the same way corpus
// "#info" lines are).
//
// __VMTEST_PROBE_DEPTH is supplied by an --include'd snippet (see
// stack_probe.sh) so one file serves every depth this measurement needs.
if (typeof __VMTEST_PROBE_DEPTH === "undefined") {
  throw new Error("stack_probe.js requires --include <depth-snippet> defining __VMTEST_PROBE_DEPTH");
}

function dive(n) {
  // Probe BEFORE recursing: the first call (n == __VMTEST_PROBE_DEPTH) fixes
  // stack_probe_first, the last (n == 0) fixes stack_probe_last, and every
  // call in between contributes exactly one JS_CallInternal frame's worth of
  // C stack between consecutive probes -- that per-call delta is what G1 is
  // measuring, not the total.
  __vmtest_stack_probe();
  if (n > 0) dive(n - 1);
}
dive(__VMTEST_PROBE_DEPTH);
print("dive depth=" + __VMTEST_PROBE_DEPTH + " done");
