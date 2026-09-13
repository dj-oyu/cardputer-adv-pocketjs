// yield_held_terminate (design sec.12.9): paired with --terminate-after N
// once JS_VMCallJob (D36) exists -- terminating while a PROMISE JOB's own
// handler is parked (JOB_HELD) must end the same way as terminating a
// host-owned SEG floor (uncatchable, no `finally`) and must not leak the
// held job's argv/aux (sec.12.6-8).
function costly(n) {
  let t = 0;
  for (let i = 0; i < n; i++) t += i;
  return t;
}
let ranFinally = false;
Promise.resolve(1).then((v) => {
  try {
    return v + costly(2100);
  } finally {
    ranFinally = true;
  }
}).then((v) => print("held-terminate", v, "finally", ranFinally));
