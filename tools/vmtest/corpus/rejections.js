// vmrun-flags: --frames 2
// Unhandled vs later-handled rejections, as guest.c drain_jobs() reports
// them: only after the queue empties, and only for promises still rejected
// without a handler at that point. Spec L1: the report time must not move
// earlier when a drain is cut by a budget.

// 1. Never handled: reported after the post-eval drain.
Promise.reject(new Error("never-handled"));

// 2. Handled a few ticks later in the same drain: NOT reported.
const late = Promise.reject(new Error("handled-same-drain"));
Promise.resolve().then(() => Promise.resolve()).then(() => {
  late.catch((e) => print("caught-late", e.message));
});

// 3. async function throwing, result dropped: reported.
(async () => { await null; throw new TypeError("async-dropped"); })();

// 4. Rejection absorbed by Promise.all: the inner promise is handled by all(),
//    the all() promise is handled by catch.
Promise.all([Promise.reject("inner"), Promise.resolve(1)]).catch((e) => print("all-caught", e));

// 5. Rejected in a then callback and the chain end is dropped: reported once
//    (only the last promise in the chain is unhandled).
Promise.resolve().then(() => { throw new RangeError("chain-end"); }).then(() => "unreached");

// 6. Rejected now, handled only in frame() #1 -- after the report. The guest
//    already reported and forgot it, so the later catch runs silently.
const handledInFrame = Promise.reject(new Error("handled-next-frame"));

// 7. Rejected with a non-Error value.
Promise.reject(42);

let n = 0;
globalThis.frame = () => {
  n++;
  print("frame", n);
  if (n === 1) {
    handledInFrame.catch((e) => print("frame-caught", e.message));
    // 8. Rejection created during a frame, unhandled at the end of its drain.
    Promise.reject(new Error("from-frame-1"));
  }
};
print("sync-end");
