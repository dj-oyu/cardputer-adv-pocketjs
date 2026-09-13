// yield_async_generator (first-version 12.13-2): the ASYNC_GENERATOR floor
// (design sec.12.4's fourth table row) -- the loop runs across `.next()`
// calls via js_async_generator_resume_next, each iteration a candidate
// MAY_YIELD section once L2c's body lands (the token written/cleared around
// that resume, sec.12.3).
async function* gen(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    sum += i;
    if (i % 500 === 0) await null;
  }
  yield sum;
}
(async () => {
  for await (const v of gen(3000)) print("async-gen", v);
})();
