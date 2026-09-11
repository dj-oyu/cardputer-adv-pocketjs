// vmrun-flags: --frames 2
// A job that itself throws (queueMicrotask callback): JS_ExecutePendingJob
// returns < 0, guest.c's drain_jobs() dumps it and returns ESP_FAIL at once,
// leaving the rest of the queue and the rejection report for the NEXT drain.
// That early return is observable and is part of the baseline.
queueMicrotask(() => print("job-1"));
queueMicrotask(() => { throw new Error("job-2-throws"); });
queueMicrotask(() => print("job-3 (runs in the frame-1 drain)"));
Promise.reject(new Error("rejected-before-throw"));
globalThis.frame = () => print("frame");
print("sync-end");
