// yield_native_reentry (first-version 12.13-2): a loop inside
// Array.prototype.sort's comparator callback. The comparator is JS reached
// through a NATIVE C reentry (sort calling back into JS) -- design sec.12.16
// notes the enclosed-call paths (apply/call/bound/Proxy/a builtin's own
// callback) stay C-recursive and are not themselves a place the OUTER call
// can yield, but the comparator's own floor is an ordinary bytecode floor
// with its own safepoints. Expected once L2c's body lands: held > 0 (the
// comparator's floor stops), stops == 0 for the surrounding call (this file
// is the doc's own example of that split).
function cost(n) {
  let t = 0;
  for (let i = 0; i < n; i++) t += (i * 7) % 13;
  return t;
}
const arr = [5, 3, 8, 1, 9, 2];
arr.sort((a, b) => (cost(300), a - b));
print("sorted", arr.join(","));
