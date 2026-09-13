// yield_flat_chain (first-version 12.13-2): a flat (L2b) call chain with a
// loop deep enough to yield partway through, including an IIFE and a method
// call that reads `this` -- both are places D33's FLAT reassembly
// (vm_resume:) has to restore correctly (cur_func for the IIFE, this_obj for
// the method) once L2c's body lands, not just re-derive from the top frame.
function inner(n) {
  let acc = 0;
  for (let i = 0; i < n; i++) acc += i;
  return acc;
}
function outer(n) {
  return inner(n) + inner(n * 2);
}
print("outer", outer(2000));
print("iife", (function (n) {
  let t = 0;
  for (let i = 0; i < n; i++) t += i * 2;
  return t;
})(1500));
const obj = {
  base: 10,
  sumTo(n) {
    let t = this.base;
    for (let i = 0; i < n; i++) t += i;
    return t;
  },
};
print("method", obj.sumTo(1800));
