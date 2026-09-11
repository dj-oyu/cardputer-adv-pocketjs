// Closures sharing variables across frames: the JSVarRef cases L2/L3 must
// keep pointing at the same slot whether the frame is alive, detached, or
// (later) moved.
function counterPair() {
  let n = 0;
  return [() => ++n, () => n, (v) => { n = v; }];
}
const [inc, get, set] = counterPair();
inc(); inc();
print("pair", get());
set(40); inc(); inc();
print("pair-set", get());

// Two closures created in different calls of the same outer frame's inner
// function, both capturing the outer variable while the outer frame is alive.
function live() {
  let shared = 1;
  function mk(m) { return () => (shared *= m); }
  const a = mk(2), b = mk(3);
  a(); b(); a();
  const seenInside = shared;
  return [seenInside, () => shared, a];
}
const [inside, peek, dbl] = live();
dbl();
print("live", inside, peek());

// Three levels: inner writes the outermost variable after both outer frames
// returned.
function l1() {
  let x = "a";
  return function l2() {
    let y = "b";
    return function l3() { x += "!"; y += "?"; return x + y; };
  };
}
const l3 = l1()();
l3();
print("nested", l3());

// Sloppy mapped arguments alias the parameter the closure captured.
function mapped(a) {
  const g = () => a;
  arguments[0] = "via-arguments";
  const r1 = g();
  a = "via-param";
  return [r1, arguments[0], g()];
}
print("mapped", mapped("orig").join(" "));
function unmapped(a) {
  "use strict";
  const g = () => a;
  arguments[0] = "via-arguments";
  return [g(), arguments[0]];
}
print("unmapped", unmapped("orig").join(" "));

// Closure over a variable of a frame that is suspended (generator), resumed
// after the closure changed it.
function* gen() {
  let v = 1;
  const bump = () => { v += 10; };
  yield bump;
  yield v;
  v++;
  yield v;
}
const it = gen();
const bump = it.next().value;
bump();
print("gen-capture", it.next().value, it.next().value);

// Closure over an async frame, changed while the frame awaits.
async function awaiting() {
  let v = "before";
  const setter = (x) => { v = x; };
  queueMicrotask(() => setter("changed-while-suspended"));
  await null;
  await null;
  return v;
}
awaiting().then((v) => print("async-capture", v));

// Closures in a loop body capturing a block-scoped const and the loop var.
const fns = [];
for (let i = 0; i < 3; i++) {
  const sq = i * i;
  fns.push(() => `${i}:${sq}`);
  i += 0;
}
print("loop", fns.map((f) => f()).join(","));

// eval-introduced variable captured by a closure (sloppy direct eval).
function evalCapture() {
  eval("var e = 'from-eval'");
  return () => e;
}
print("eval-capture", evalCapture()());

// Recursion where every frame captures its own n and a shared accumulator.
function chain(n, acc) {
  if (n === 0) return acc;
  acc.push(() => n);
  return chain(n - 1, acc);
}
print("chain", chain(5, []).map((f) => f()).join(""));
