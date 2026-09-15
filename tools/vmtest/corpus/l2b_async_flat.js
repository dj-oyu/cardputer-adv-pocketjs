// L2b-async (docs/vm-L2-design.md sec.10.1, D32-D34): an async function
// called from JS runs its first synchronous stretch as a flat frame in the
// caller's C activation, and the promise it returns travels through the
// caller's func slot instead of a C return value. Every line is a place
// where that path could differ from the upstream C call and must not: the
// promise's identity and class, the order of the synchronous stretch against
// the caller's next statement, a throw before the first await becoming a
// rejected promise (never an exception in the caller's try), `this` for a
// method call, rest / arguments / default parameters read in the body, async
// arrows and class async methods, return await, the function name in
// Error().stack from inside the body, tail position (`return f()`), async
// calling async, a generator floor calling async from a default initializer
// (sec.10.3), recursion that resumes after an await, the C-recursive wrapped
// paths (apply / call / bound / Proxy / Reflect / a builtin's callback), and
// a promise that is dropped without ever being observed. Blessed on
// asan-recur (the upstream path); the flat variants must match it byte for
// byte. Deep recursion is deliberately absent: that is budget_probe.sh's
// deep_async_recursion.
const log = [];
async function plain(x) { log.push("plain " + x); return x + 1; }
const p = plain(1);
log.push("after-call");
print("promise-class", p instanceof Promise, Object.getPrototypeOf(p) === Promise.prototype, typeof p.then);
print("sync-order", log.join(","));
p.then((v) => print("resolved", v));
print("same-promise", plain(2) !== plain(2), (function () { const q = plain(3); return q === q; })());

// A throw before the first await never reaches the caller's try.
async function throwsEarly() { throw new TypeError("early"); }
let syncCaught = "none";
let q;
try { q = throwsEarly(); } catch (e) { syncCaught = e.constructor.name; }
print("throw-sync", syncCaught, q instanceof Promise);
q.catch((e) => print("throw-rejected", e.constructor.name, e.message));
async function throwsLate() { await 0; throw new RangeError("late"); }
throwsLate().catch((e) => print("throw-late", e.constructor.name, e.message));
async function catchesOwn() { try { throw new Error("own"); } catch (e) { return "caught-" + e.message; } }
catchesOwn().then((v) => print("catch-inside", v));
async function finallyRuns() { try { return "ret"; } finally { log.push("finally"); } }
finallyRuns().then((v) => print("finally", v));
print("finally-sync", log.includes("finally"));

// this / rest / arguments / defaults inside the body.
const o = { v: 7, async m(a, ...r) { return this.v + a + r.length + arguments.length; } };
o.m(1, 2, 3).then((v) => print("method-this", v));
async function defs(a = plain(10), ...r) { return [(await a), r.length, arguments.length]; }
defs().then((v) => print("defaults", JSON.stringify(v)));
defs(5, 6, 7).then((v) => print("defaults-args", JSON.stringify(v)));
async function withThis() { return this === undefined ? "undef" : typeof this; }
withThis().then((v) => print("plain-this", v));
async function mapped(a, b) { arguments[0] = 9; return a + b; }
mapped(1, 2).then((v) => print("mapped-args", v));

// async arrow, class methods, return await.
const arrow = async (x) => x * 2;
arrow(21).then((v) => print("arrow", v));
class K {
  constructor() { this.n = 3; }
  async get() { return this.n; }
  static async s() { return "static"; }
  async ra() { return await plain(100); }
}
new K().get().then((v) => print("class-method", v));
K.s().then((v) => print("class-static", v));
new K().ra().then((v) => print("return-await", v));

// Error().stack from inside the synchronous stretch names the function and
// the caller below it.
async function named() { return new Error("x").stack; }
function callsNamed() { return named(); }
callsNamed().then((s) => print("stack-names", s.includes("named"), s.includes("callsNamed"), s.split("\n").length > 2));

// Tail position: the caller hands the promise back as its own return value.
function tail() { return plain(41); }
const tp = tail();
print("tail-promise", tp instanceof Promise);
tp.then((v) => print("tail-value", v));
const to = { t() { return o.m(0); } };
to.t().then((v) => print("tail-method", v));

// async from async: a flat frame inside a flat frame, then one that awaits.
async function inner(n) { log.push("inner " + n); return n; }
async function outer() { const a = inner(1); log.push("outer-mid"); const b = await inner(2); log.push("outer-resumed"); return (await a) + b; }
log.length = 0;
const op = outer();
log.push("after-outer");
print("nested-order", log.join(","));
op.then((v) => print("nested", v, log.slice(4).join(",")));

// A generator floor calling async from a default initializer.
function* gen(a = plain(0), ...r) { yield [a instanceof Promise, r.length]; }
print("gen-floor", JSON.stringify(gen().next().value), JSON.stringify(gen(1, 2).next().value));

// Recursion that resumes after an await: each level's synchronous stretch
// ends at its await and the rest runs from a job.
async function count(n) { if (n === 0) return 0; return 1 + await count(n - 1); }
count(50).then((v) => print("recurse-await", v));
async function fan(n) { if (n === 0) return 1; const [a, b] = await Promise.all([fan(n - 1), fan(n - 1)]); return a + b; }
fan(5).then((v) => print("fan-out", v));

// Wrapped paths stay C-recursive and must agree with the flat ones.
plain.apply(null, [5]).then((v) => print("apply", v));
plain.call(null, 6).then((v) => print("call", v));
o.m.bind({ v: 100 })(1).then((v) => print("bound", v));
new Proxy(plain, {})(7).then((v) => print("proxy", v));
Reflect.apply(plain, null, [8]).then((v) => print("reflect", v));
Promise.all([1, 2, 3].map(async (x) => x * 10)).then((v) => print("map-async", v.join()));
print("sort-async", [3, 1, 2].sort((a, b) => { plain(a); return a - b; }).join());

// Not a constructor; the class check tells async apart from normal.
try { new plain(); } catch (e) { print("new-async", e.constructor.name); }
print("kind", Object.getPrototypeOf(plain) === Object.getPrototypeOf(async function () {}), typeof plain);

// A promise nobody keeps: the creator's reference alone must free it.
(async () => { await 0; })();
async function dropped() { return "gone"; }
dropped();
print("sync-log", log.join(","));
