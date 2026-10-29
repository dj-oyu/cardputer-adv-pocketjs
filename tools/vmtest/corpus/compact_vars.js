// vmrun-flags: --harden
// Compact variables (docs/vm/compact-vars.md): in a hardened context a
// function keeps, per argument and local, only the JSVarRef slot of a
// captured variable. Every way a closure reaches a variable must still find
// the same slot, alive or detached: arguments, locals, block scopes in
// loops, chains of closures, generators and async functions across a
// suspension, mapped arguments, class fields, and the TDZ. Sloppy, so the
// arguments object is mapped.
function pair(a) {
  let n = a;
  return [() => ++n + a, v => { a = v; }];
}
const [inc, setA] = pair(10);
print("arg+local", inc(), inc());
setA(100);
print("arg written", inc());

// A block-scoped variable per iteration, captured, then the loop ends.
const fs = [];
for (let i = 0; i < 4; i++) { const sq = i * i; fs.push(() => i + ":" + sq); }
print("loop", fs.map(f => f()).join(","));

// Captured and uncaptured locals side by side; only some are slots.
function mixed(x) {
  let a = 1, b = 2, c = 3, d = 4;
  const keep = () => a + c;
  b += x; d *= x;
  a = b + d;
  return keep() + b + d;
}
print("mixed", mixed(3));

// Three levels: the inner closure reaches a local two frames up.
function outer() {
  let o = 'o';
  return function middle() {
    let m = 'm';
    return () => o + m + (o = o + '!', m = m + '?', '');
  };
}
const inner = outer()();
print("chain", inner(), inner());

// Recursion: every frame captures its own n.
function rec(n, acc) { if (n === 0) return acc; acc.push(() => n); return rec(n - 1, acc); }
print("rec", rec(5, []).map(f => f()).join(""));

// A generator's local captured across yields.
function* gen() {
  let g = 0;
  const peek = () => g;
  while (g < 3) { g++; yield peek(); }
}
print("gen", [...gen()].join(","));

// An async function's local captured and changed while it is suspended.
async function asyncCapture() {
  let v = 'before';
  const read = () => v;
  const p = Promise.resolve().then(() => { v = 'changed-while-suspended'; });
  await p;
  return read();
}
asyncCapture().then(r => print("async", r));

// Sloppy mapped arguments go through the same slots.
function sloppyMapped(x) { return (function (y) { arguments[0] = 'via-arguments'; return y; })(x); }
print("arguments", sloppyMapped('orig'));

// Class fields close over the constructor's scope.
function makeClass(k) {
  let count = 0;
  return class { f = () => k + (++count); };
}
const C = makeClass('k'), c1 = new C(), c2 = new C();
print("class", c1.f(), c2.f(), c1.f());

// TDZ: a captured let read before its declaration.
try {
  const early = () => late;
  early();
  let late = 1;
} catch (e) { print("tdz", e.constructor.name, e.message); }
try { (function () { tdzLocal; let tdzLocal = 2; })(); }
catch (e) { print("tdz local", e.constructor.name, e.message); }
