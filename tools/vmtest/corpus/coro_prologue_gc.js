// vmrun-flags: --test262
// Companion to coro_closure_gc.js (7955cfd49e backport, backlog #13): var_refs
// created in a generator's PROLOGUE -- before OP_initial_yield, i.e. before
// the generator object exists -- by mapped `arguments` and by default-
// parameter closures. The port marks a frame as a coroutine only once its
// owner object exists, so these stay ordinary open var_refs (upstream does
// the same). This file records what that means: the prologue closures and
// arguments keep working across collections while the generator is also
// held normally, and releasing everything afterwards frees cleanly (LSan).
function* g1(a, f = () => a) { const args = arguments; yield f(); yield args[0]; }
const it1 = g1(41);
$262.gc();
print("gen-default-closure", it1.next().value);
$262.gc();
print("gen-arguments", it1.next().value);

async function* ag1(a, f = () => a) { const args = arguments; yield f(); yield args[0]; }
const it2 = ag1(42);
$262.gc();
it2.next().then((r) => { print("agen-default-closure", r.value); $262.gc(); return it2.next(); })
  .then((r) => print("agen-arguments", r.value));

// Escaped from the prologue, generator otherwise unreachable.
globalThis.esc = null;
(function () {
  function* g2(a, f = (globalThis.esc = () => a)) { yield a; }
  g2(43);
})();
$262.gc();
print("gen-prologue-escaped", esc());

// Prologue var_refs (ordinary) and body var_refs (coroutine) on one frame,
// the generator reachable only through a cycle behind a body closure. Before
// the port the body edge was invisible and the generator was collected (ASan
// UAF); after it, a kind set before the owner existed would dereference NULL.
globalThis.esc3 = null;
(function () {
  let it;
  function* g3(a, f = () => a) { const o = { f, args: arguments }; o.it = it; globalThis.esc3 = () => o; yield 1; yield a + o.args[0]; }
  it = g3(5); it.next(); it = null;
})();
$262.gc();
const o3 = esc3();
print("gen-mixed-cycle", o3.f(), o3.it.next().value);

globalThis.esc4 = null;
(function () {
  let it;
  async function* ag3(a, f = () => a) { const o = { f, args: arguments }; o.it = it; globalThis.esc4 = () => o; yield 1; yield a + o.args[0]; }
  it = ag3(6); it.next(); it = null;
})();
// collect in a job: once the first next() has settled (see coro_closure_gc)
Promise.resolve().then(() => {}).then(() => {}).then(() => {
  $262.gc();
  const o4 = esc4();
  return o4.it.next().then((r) => print("agen-mixed-cycle", o4.f(), r.value));
});
