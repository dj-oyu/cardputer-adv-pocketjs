// yield_call_on_chain: paired with --call-while-suspended (design sec.12.5,
// D19r) once L2c's body exists -- a JS_Call or JS_ExecutePendingJob issued
// WHILE a chain is parked must be refused with "VM suspended" rather than
// stacking a second activation above the held one. Until then the flag only
// probes JS_VMSuspended() (always false) and prints a note; this file
// records the ordinary (unyielded) chain it will eventually be layered onto.
function step(n) {
  let t = 0;
  for (let i = 0; i < n; i++) t += i;
  return t;
}
function chain(n) {
  return step(n) + step(n + 1);
}
print("chain", chain(1700));
