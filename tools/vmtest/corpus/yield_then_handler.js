// yield_then_handler (design sec.12.9, D36): an ORDINARY function used as a
// .then handler is itself a MAY_YIELD floor once L2c's body lands (D17r-1's
// JS_VMCallJob token) -- the loop inside the handler can suspend the
// PROMISE JOB itself (JOB_HELD), not just a bytecode call chain, and the
// derived promise must still resolve with the handler's return value after
// the resume+tail (sec.12.6-5). The chain below fixes an exact FIFO order
// (sec.12.6-6) across three .then links, a queueMicrotask, and a fresh
// Promise.resolve().then; a reorder anywhere would change the "order" line.
// queueMicrotask(() => ({})) is here so LSan sees the object a resolved
// microtask job returns get freed (the release sec.12.4 requires from
// JS_VMResume's tail path).
const order = [];
function costly(n) {
  let t = 0;
  for (let i = 0; i < n; i++) t += i;
  return t;
}
Promise.resolve(1)
  .then((v) => { order.push("then1:" + costly(2600)); return v + 1; })
  .then((v) => { order.push("then2:" + v); return v + 1; })
  .then((v) => { order.push("then3:" + v); });
queueMicrotask(() => order.push("microtask"));
queueMicrotask(() => ({}));
Promise.resolve().then(() => order.push("resolve-then"));
// Polls rather than guessing a fixed chain depth: every entry above is
// queued from code that has already run by the time this file finishes
// executing, so this settles in a fixed, deterministic number of turns for
// a given interpreter -- which is all --bless needs.
(function afterAll() {
  if (order.length < 5) { Promise.resolve().then(afterAll); return; }
  print("order", order.join(","));
})();
