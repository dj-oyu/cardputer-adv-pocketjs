// VM_PROBE L0 workload, USB-only: one frame = building and calling a batch
// of closures, so var-ref boxing (JSVarRef -- the other private struct L0
// measures the size of) is on the hot path rather than plain locals.
function make(n) {
  let x = n;
  return () => (x += 1, x);
}
globalThis.frame = () => {
  let total = 0;
  for (let i = 0; i < 500; i++) { const f = make(i); total += f() + f(); }
  return total;
};
