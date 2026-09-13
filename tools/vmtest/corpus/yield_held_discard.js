// yield_held_discard (design sec.12.9): paired with --discard-after N once
// JS_VMCallJob (D36) exists -- ending the session while a promise job's own
// handler is parked (JOB_HELD) must free its argv/e/aux (sec.12.6-7) with no
// ASan/LSan report, the same guarantee yield_discard checks for a
// host-owned SEG floor.
function costly(n) {
  let t = 0;
  for (let i = 0; i < n; i++) t += i;
  return t;
}
const kept = { tag: "kept" };
Promise.resolve(1)
  .then((v) => (kept ? v + costly(1900) : 0))
  .then((v) => print("held-discard", v));
