// VM_PROBE L0 workload, USB-only: one frame = one call chain 3/4 as deep as
// the 20 KiB stack_limit allows, measured once at start. A fixed 400 threw
// every frame: JS_CallInternal is entry 336 B + alloca on Xtensa (~50 deep).
// G1 device side (vmprobe.h's vmprobe_depth_stack_sample): sample the ui
// task's stack high-water mark at power-of-two depths on the way down, so a
// handful of points span whatever max turns out to be without knowing it in
// advance (VMPROBE_DEPTH_CAP is small on purpose -- see vmprobe.c).
let max = 0;
function probe(n) {
  max = n;
  if ((n & (n - 1)) === 0 && typeof __vmprobe_stack_sample === "function")
    __vmprobe_stack_sample(n);
  probe(n + 1);
}
try { probe(1); } catch (e) {}
const depth = (max * 3) >> 2;
function descend(n) { return n <= 0 ? 0 : 1 + descend(n - 1); }
globalThis.frame = () => descend(depth);
