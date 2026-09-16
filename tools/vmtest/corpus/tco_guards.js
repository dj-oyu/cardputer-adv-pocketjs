"use strict";
function protectedCall(n) {
  if (!n) throw 42;
  try { return protectedCall(n - 1); }
  catch (error) { return [n, error].join(":"); }
}
print("protected", protectedCall(20));
let effects = 0;
function finalCall(n) {
  try { return n ? finalCall(n - 1) : 7; }
  finally { effects++; }
}
print("finally", finalCall(20), effects);
const closures = [];
function catchClosure(n) {
  if (!n) return 9;
  try { throw n; }
  catch (error) {
    closures.push(() => error);
    return catchClosure(n - 1);
  }
}
print("catch-closure", catchClosure(20), closures.map(f => f()).join(","));
function nativeTail(x) { return Math.abs(x); }
const target = function (x) { return this.k + x; };
const bound = target.bind({k: 3});
const proxy = new Proxy(target, {apply(fn, self, args) { return Reflect.apply(fn, {k: 5}, args); }});
function indirect(fn) { return fn(4); }
print("wrapped", nativeTail(-3), indirect(bound), indirect(proxy));
function many(a,b,c,d,e,f,g,h,i) { return [a,b,c,d,e,f,g,h,i].join(":"); }
function manyTail() { return many(1,2,3,4,5,6,7,8,9); }
print("many", manyTail());
function direct() { const local = 19; return eval("local + 2"); }
print("direct-eval", direct());
let closed = 0;
const iterable = { [Symbol.iterator]() { return {
  next() { return {value: 1, done: false}; },
  return() { closed++; return {done: true}; }
}; }};
function iteratorTail() { for (const n of iterable) return nativeTail(-n); }
print("iterator", iteratorTail(), closed);
// More arguments than the reuse scratch capacity: retain the old budget.
function wide(n,a,b,c,d,e,f,g,h) {
  return n ? wide(n-1,a,b,c,d,e,f,g,h) : 42;
}
try { wide(100000,1,2,3,4,5,6,7,8); }
catch (error) { print("wide-budget", error instanceof RangeError); }
// Exceed a standard segment on both 32-bit and 64-bit builds. Keep every
// local live through the call, so the compiler cannot erase the frame.
const declarations = Array.from({length: 600}, (_,i) => "v"+i+"="+i).join(",");
const captures = Array.from({length: 600}, (_,i) => "v"+i).join("+");
const big = Function("return function big(n){'use strict';let "+declarations+
  ";globalThis.bigClosure=()=>"+captures+";return n?big(n-1):42}")();
try { big(100000); }
catch (error) { print("big-budget", error instanceof RangeError); }
print("big-capture", bigClosure());
print("after-fallback", protectedCall(3), nativeTail(-42));
