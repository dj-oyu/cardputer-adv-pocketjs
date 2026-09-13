// yield_thenable (design sec.12.9): a `then` METHOD (not a Promise) driving
// resolution -- js_promise_resolve_thenable_job. The loop lives inside that
// method, so once L2c's body lands its floor is the thenable job's own
// handler, and D36's `aux` (the resolve function `then` was called with)
// must survive a suspend/resume of THIS job without an extra free or a
// leak.
function costly(n) {
  let t = 0;
  for (let i = 0; i < n; i++) t += i;
  return t;
}
const thenable = {
  then(resolve) {
    const v = costly(2400);
    resolve(v);
  },
};
Promise.resolve(thenable).then((v) => print("thenable", v));
