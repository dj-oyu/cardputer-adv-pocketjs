// vmrun-flags: --budget-jobs 8 --runaway-jobs 2000
// L1 invariant 8 (docs/vm-L1-design.md sec.5): a job that queues another job
// forever. The queue length never exceeds one, every job finishes in
// microseconds, and no wall-clock deadline is ever crossed -- so the old
// 250 ms interrupt guard cannot see this at all. What sees it is what the
// LOGICAL drain has spent: on the board, microseconds summed over the drain
// and its continuations (VM_RUNAWAY_US); here, where the clock is deliberately
// never read, the job total that is the firmware's dead-clock fallback. 2,000
// rather than the shipping 100,000 only so the case runs in a moment; the
// predicate is the same one, and the thing it must NOT catch is
// budget_honest_long_chain.js.
//
// The end is a session end at a JOB BOUNDARY, not an uncatchable
// InternalError: no JavaScript frame is live when it happens, so nothing gets
// a finally skipped and no await is left pending forever. The queue is then
// discarded unrun, which is what JS_FreeRuntime does anyway; LSan is what
// checks that discarding it leaks nothing.
print('arming');
function f() { Promise.resolve().then(f); }
f();
print('armed');
