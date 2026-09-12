// vmrun-flags: --budget-jobs 4 --stop-turns 20
// L1 invariant 9 (docs/vm-L1-design.md sec.3.2): the session ends while the
// queue still has work. --stop-turns is vmrun's stand-in for app_stop(): after
// 20 continuation turns the run ends at a job boundary with the remaining
// 1000-step chain never run.
//
// Two things are being fixed in place. The queue is discarded UNRUN --
// JS_FreeRuntime's own behaviour, and the same choice pocket_api_reset()
// already makes for in-flight promises -- and "#info jobs_dropped=1" records
// it as the single bit it can honestly be (counting would need a hook inside
// JS_EnqueueJob, which is a VM change L1 may not make).
//
// The rejection below is the second thing: it is tracked as unhandled and is
// still on the tracker's list when the session ends. It must NOT be reported.
// A catch for it could have been in any of the dropped jobs, so reporting
// would be a guess. Expected output therefore contains no
// "Unhandled Promise rejection" line at all, and the exit code is 0: ending
// with a queue is an ordinary end, not a failure.
print('start');
Promise.reject(new Error('dropped-unreported'));
let p = Promise.resolve();
for (let i = 0; i < 1000; i++) p = p.then(() => {});
p.then(() => print('NEVER: the chain finished'));
print('queued');
