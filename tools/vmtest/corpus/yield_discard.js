// yield_discard: paired with --discard-after N (design sec.12.12, D26r) once
// L2c's body exists -- ending the session while a chain is parked has to
// free everything on it (JS_VMDiscard, at JS_FreeRuntime's teardown) with no
// ASan/LSan report. A closure and an object are kept alive across the loop
// so a leaked frame would hold them past teardown and LSan would see it.
function build(n) {
  const kept = { tag: "kept" };
  let sum = 0;
  for (let i = 0; i < n; i++) sum += i;
  return () => sum + (kept ? 0 : 1);
}
const f = build(2200);
print("discard", f());
