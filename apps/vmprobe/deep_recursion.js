// VM_PROBE L0 workload, USB-only: one frame = one call chain 3/4 as deep as
// the 20 KiB stack_limit allows, measured once at start. A fixed 400 threw
// every frame: JS_CallInternal is entry 336 B + alloca on Xtensa (~50 deep).
let max = 0;
function probe(n) { max = n; probe(n + 1); }
try { probe(1); } catch (e) {}
const depth = (max * 3) >> 2;
function descend(n) { return n <= 0 ? 0 : 1 + descend(n - 1); }
globalThis.frame = () => descend(depth);
