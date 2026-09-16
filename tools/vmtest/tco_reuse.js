// Run separately: requires the opt-in TCO build; normal builds overflow.
"use strict";
const N = globalThis.TCO_DEPTH || 100000;
const saved = [];
function a(n, value) {
  if (n % 10000 === 0) saved.push(() => value);
  if (!n) return value;
  return b(n - 1, value + 1, 7);
}
function b(n, value, extra) {
  if (extra !== 7) throw new Error("extra");
  return a(n, value);
}
print("mutual", a(N, 0), saved.map(f => f()).join(","));
const obj = {
  tag: 19,
  f(n, ...args) {
    if (this !== obj || args.length !== 2 || args[1] !== 4)
      throw new Error("method arguments");
    if (!n) return this.tag + args[0];
    return obj.f(n - 1, args[0] + 1, 4);
  }
};
print("method", obj.f(N, 0, 4));
function invoke(n, next) {
  if (!n) return next();
  // The only owner of this closure becomes the replacement frame.
  return (function fresh(m) { return invoke(m, () => 31); })(n - 1);
}
print("closure", invoke(N / 10, () => 0));
function boom(n) { if (!n) throw new Error("bottom"); return boom(n - 1); }
try { boom(N); } catch (e) { print("throw", e.message); }
function smaller(n, x = 8) { if (!n) return x; return smaller(n - 1); }
print("default", smaller(N, 99));
print("recovered", a(N / 10, 0));
