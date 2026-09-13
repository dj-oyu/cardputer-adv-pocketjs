// yield_async_in_chain (design sec.12.9/12.8): a call chain with a flat
// async frame IN THE MIDDLE -- SEG floor -> flat SEG -> flat async -> flat
// SEG, yielding at the top. Once L2c's body lands this is what exercises
// D21r's split GC ownership (the walk marks the SEG frames; the flat async
// frame's own JSAsyncFunctionData marks itself) -- paired with
// --gc-on-yield.
function leaf(n) {
  let t = 0;
  for (let i = 0; i < n; i++) t += i;
  return t;
}
async function middle(n) {
  const v = leaf(n);
  await null;
  return v;
}
function outer(n) {
  return middle(n);
}
function top(n) {
  return outer(n);
}
top(2900).then((v) => print("async-in-chain", v));
