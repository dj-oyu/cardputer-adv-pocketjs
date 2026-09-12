// L2a corpus (docs/vm-L2-design.md sec.1.1 #6, "closures holding a frame
// while its segment is returned"): closures.js fixes JSVarRef semantics in
// general; this file specifically interleaves unrelated deep recursion
// (segment churn) between every read of a captured variable, so a segment
// returned-and-reused bug has a concrete chance to stomp the captured slot
// before the next read.
function churn(n) { return n === 0 ? 0 : 1 + churn(n - 1); }

function makeAt(n) {
  let v = n;                      // captured
  function deeper(m) { return m === 0 ? 0 : 1 + deeper(m - 1); }
  deeper(n);                      // burn depth under this frame before it returns
  return { get: () => v, bump: () => (v += 1) };
}
const cap = makeAt(500);
churn(2000);
print("closure-before", cap.get());
cap.bump();
churn(2000);
print("closure-after", cap.get());

// The capturing frame is itself several levels deep when IT returns, so its
// segment window overlaps ancestors that are still on the (still-growing)
// call chain at the moment it is reclaimed.
function outer(n) {
  if (n === 0) {
    let v = 0;
    const inc = () => ++v;
    return inc;
  }
  const inner = outer(n - 1);
  churn(500);
  return inner;
}
const inc2 = outer(300);
churn(2000);
print("nested-closure", inc2(), inc2(), inc2());

// Many independent closures from many independent (now-returned) frames,
// read back out of creation order, after further churn -- a stale segment
// reused for one closure's frame must not leak into another's.
const makers = [];
for (let i = 0; i < 50; i++) {
  makers.push((function (seed) {
    let v = seed;
    churn(200);
    return () => v;
  })(i * 7));
}
churn(3000);
const readBack = [];
for (let i = makers.length - 1; i >= 0; i--) readBack.push(makers[i]());
print("many-closures", readBack.join(","));
