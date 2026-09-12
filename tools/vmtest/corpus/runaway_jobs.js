// vmrun-flags: --budget-jobs 8 --runaway-turns 30
// L1 invariant 8 (docs/vm-L1-design.md sec.5): a job that queues another job
// forever. The queue length never exceeds one, every job finishes in
// microseconds, and no wall-clock deadline is ever crossed -- so the old
// 250 ms interrupt guard cannot see this at all. What sees it is the count of
// consecutive turns that ended with the queue still non-empty.
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
