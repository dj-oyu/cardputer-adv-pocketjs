// vmrun-flags: --test262
// Regression for upstream quickjs-ng 7955cfd49e ("Fix use-after-free of a
// suspended coroutine reachable only via a closure"), docs/vm/backlog.md #13.
// A closure capturing a coroutine local holds an open var_ref into the
// coroutine's heap frame. That edge used to be invisible to the cycle
// collector, so a suspended coroutine reachable only through such a closure
// (or through an escaped mapped `arguments`) was freed while live, and using
// the closure or resuming the coroutine touched freed memory (ASan:
// heap-use-after-free on every build variant before the fix).
// Adapted from upstream tests/suspended-{coroutine-closure,coroutine-mapped-
// arguments,generator-closure}-gc.js; $262.gc() runs a full collection.

// async function, closure over a local
globalThis.leaked = null;
(function () {
  async function step() {
    const d = Promise.withResolvers();
    globalThis.leaked = () => d;
    await d.promise;
  }
  step();
})();
$262.gc();
print("async-closure", typeof leaked().resolve);

// async function, escaped mapped arguments
const AsyncFunction = (async function () {}).constructor;
const step2 = AsyncFunction("a",
  "globalThis.leaked2 = () => arguments; const d = Promise.withResolvers(); await d.promise;");
step2(123);
$262.gc();
print("async-arguments", leaked2()[0]);

// generator in a cycle through a captured local, resumed after the GC
globalThis.leaked3 = null;
(function () {
  let g;
  function* gen() { const o = {}; o.g = g; globalThis.leaked3 = () => o; yield 1; yield 2; }
  g = gen(); g.next(); g = null;
})();
$262.gc();
const o = leaked3();
print("generator-cycle", o.g !== null && o.g !== undefined, o.g.next().value);

// async generator, closure over a local
globalThis.leaked4 = null;
(function () {
  async function* ag() {
    const box = { v: 7 };
    globalThis.leaked4 = () => box;
    await new Promise(() => {});
  }
  ag().next();
})();
$262.gc();
print("async-generator-closure", leaked4().v);

// async generator in a cycle through a captured local, resumed after the GC.
// The case above stays alive through its pending await either way; this one
// collects in a job, once the first next() has settled and the generator
// sits at a yield with no request holding it (UAF before the port).
globalThis.leaked5 = null;
(function () {
  let g;
  async function* ag() { const box = { v: 7 }; box.g = g; globalThis.leaked5 = () => box; yield 1; yield 2; }
  g = ag(); g.next(); g = null;
})();
Promise.resolve().then(() => {}).then(() => {}).then(() => {
  $262.gc();
  const b = leaked5();
  return b.g.next().then((r) => print("async-generator-cycle", b.v, r.value));
});
