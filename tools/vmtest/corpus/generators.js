// vmrun-flags: --frames 1
// Generators and async generators: the heap-saved frames (async_func_*),
// including return()/throw() injected at a yield inside try/finally.
const out = [];
const L = (...a) => out.push(a.join(" "));

function* basic() { const x = yield 1; L("basic got", x); const y = yield x * 2; return x + y; }
let g = basic();
L("b", JSON.stringify(g.next("ignored")), JSON.stringify(g.next(5)), JSON.stringify(g.next(7)), JSON.stringify(g.next()));

function* guarded() {
  try { yield "a"; yield "b"; }
  finally { L("guarded finally"); yield "from-finally"; L("after finally yield"); }
}
g = guarded();
L("r1", JSON.stringify(g.next()));
L("r2", JSON.stringify(g.return("R")));   // finally runs, its yield suspends the return
L("r3", JSON.stringify(g.next()));        // completes the pending return
L("r4", JSON.stringify(g.next()));

g = guarded();
g.next();
try { g.throw(new Error("T")); } catch (e) { L("throw escaped", e.message); }

function* catcher() {
  while (true) {
    try { yield "wait"; }
    catch (e) { L("caught inside", e); if (e === "stop") return "stopped"; }
  }
}
g = catcher();
g.next();
L("t1", JSON.stringify(g.throw("once")));
L("t2", JSON.stringify(g.throw("stop")));
L("t3", JSON.stringify(g.next()));

// throw()/return() on a not-yet-started generator never enter the body.
function* never() { L("never entered"); yield 1; }
g = never();
L("ret-unstarted", JSON.stringify(g.return(9)));
g = never();
try { g.throw("x"); } catch (e) { L("throw-unstarted", e); }

// yield* forwards next/throw/return to the inner iterator.
function* inner() {
  try { const v = yield "i1"; L("inner got", v); yield "i2"; }
  catch (e) { L("inner caught", e); yield "i-recovered"; }
  finally { L("inner finally"); }
  return "inner-ret";
}
function* outer() { const r = yield* inner(); L("outer saw", r); yield "o-last"; }
g = outer();
L("d1", g.next().value, g.next("V").value, g.throw("E").value, g.next().value, g.next().done);
g = outer();
g.next();
L("d-return", JSON.stringify(g.return("early")));

// yield* over a hand-written iterator without throw(): TypeError, iterator closed.
const plain = {
  [Symbol.iterator]() { return this; },
  next() { return { value: "p", done: false }; },
  return() { L("plain return called"); return {}; },
};
function* overPlain() { yield* plain; }
g = overPlain();
g.next();
try { g.throw("x"); } catch (e) { L("no-throw-method", e.constructor.name); }

// Re-entrancy guard: a running generator cannot be resumed from inside.
function* selfRef() { try { g2.next(); } catch (e) { L("reentry", e.constructor.name); } yield 1; }
const g2 = selfRef();
g2.next();

// Spread, destructuring, for-of early exit calling return().
function* nums() { try { yield 1; yield 2; yield 3; } finally { L("nums closed"); } }
L("spread", [...nums()].join());
const [first] = nums();
L("destructure", first);
for (const n of nums()) { if (n === 2) break; }

// Async generators: queued requests, for await, return during await.
async function* ag() {
  try {
    yield 1;
    const v = await Promise.resolve(2);
    yield v;
    yield Promise.resolve(3);   // yield awaits its operand in async generators
  } finally {
    L("ag finally");
    await null;
    L("ag finally after await");
  }
}
(async () => {
  const it = ag();
  // Three next() calls queued before the first settles.
  const ps = [it.next(), it.next(), it.next()];
  for (const p of ps) L("ag queued", JSON.stringify(await p));
  L("ag return", JSON.stringify(await it.return("done")));
  L("ag after", JSON.stringify(await it.next()));

  let sum = 0;
  for await (const v of ag()) sum += v;
  L("for-await", sum);

  for await (const v of ag()) { if (v === 2) break; }
  L("for-await-break-done");

  const it2 = ag();
  await it2.next();
  try { await it2.throw(new Error("into-ag")); } catch (e) { L("ag throw", e.message); }

  async function* rejecting() { yield 1; throw new Error("ag-body"); }
  try { for await (const v of rejecting()) L("rej got", v); } catch (e) { L("ag rejects", e.message); }

  // for await over a sync iterable of promises.
  let s2 = "";
  for await (const v of [Promise.resolve("x"), "y", Promise.resolve("z")]) s2 += v;
  L("for-await-sync", s2);
})();

globalThis.frame = () => { out.forEach((s, i) => print(i, s)); out.length = 0; };
