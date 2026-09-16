// Regression for upstream quickjs-ng d98ff101c6 ("Fix use-after-free from
// Array `.length` grow + `push`"). Growing `.length` on a fast array leaves it
// fast with u.array.count still at the old element count. js_array_push's fast
// path took the new index from `.length` and stored past `count`, then set
// `count` to the new length: the slots in between were never initialised, so
// reads and the GC saw garbage JSValues. The fast path now requires
// `.length == count` and otherwise falls back to the generic path.
const keep = [];
for (let i = 0; i < 30; i++) {
  const a = [0];
  a.length = 245;
  a.push(2);
  keep.push(a);
}
let ok = true;
for (const a of keep) {
  ok = ok && a.length === 246 && a[0] === 0 && a[245] === 2 &&
       !a.hasOwnProperty(1) && a[100] === undefined;
}
print("push-after-length-grow", ok);
const b = [1, 2];
b.length = 5;
print("push-return", b.push(9), JSON.stringify(b), Object.keys(b).join());
