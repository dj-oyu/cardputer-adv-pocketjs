// yield_async_frame (first-version 12.13-2): the ASYNC floor (design
// sec.12.4's third table row) -- the loop here runs AFTER the await, from
// js_async_function_resolve_call's resumption (the await-return token, not
// the initial synchronous call), so this is what exercises the async-owner
// row of D18r's table rather than the flat-async row yield_async_flat does.
async function work(n) {
  await null;
  let sum = 0;
  for (let i = 0; i < n; i++) sum += i;
  return sum;
}
work(4000).then((v) => print("async-frame", v));
