// yield_async_flat (design sec.12.9): an async function CALLED FROM JS runs
// its first synchronous stretch as a flat frame (D31/D33) -- the loop here
// sits in that stretch, before the first await, so once L2c's body lands it
// yields as a flat CHILD of the caller (its MAY_YIELD bit copied down at
// push, sec.12.3) rather than needing an ASYNC floor of its own the way
// yield_async_frame's post-await loop does. The await afterwards keeps the
// before/after ordering observable in the sync-log line.
async function work(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) sum += i;
  await null;
  return sum + 1;
}
work(3500).then((v) => print("resolved", v));
print("after-call");
