// L2b (docs/vm-L2-design.md sec.10): JS-to-JS calls that run in one C
// activation must be indistinguishable from the C-recursive ones. Every line
// here is a place where the flat return rebuilds a caller local from the
// frame chain, and the value it prints depends on that rebuild being right:
// argv / argc after a flat call made from a default parameter (D11: the
// argc that was PASSED, and argv beyond the declared parameters), `this`
// and new.target read after a flat call, the return fix-up of each call
// shape (plain / method / tail / tail method, D8), exceptions and finally
// unwinding across many flat frames, the generator / async floors (H6),
// and every wrapped path that still recurses in C (constructor, apply,
// Proxy, bound, getter, sort). The same bytes are expected from the alloca,
// segframes-recursive and flat variants.
function g() { return 42; }

// OP_rest and `arguments` read argv / argc AFTER the default-parameter call
// returned: they must see what the caller passed, not what f declared.
function f(a = g(), ...r) { return [a, r.length, arguments.length]; }
print("rest-after-default", JSON.stringify(f()), JSON.stringify(f(1, 2, 3)));
function extra(a) { g(); return arguments.length + "/" + arguments[3]; }
print("argv-beyond-declared", extra(1, 2, 3, 4));
print("from-native-more-args", [10, 20].map(function (x, i, arr) { g(); return arguments.length + ":" + arguments[2].length; }).join());

// this / new.target after a flat call in the same frame.
class C { constructor(a = g()) { this.k = new.target === C; this.a = a; } }
print("ctor-default", new C().k, new C().a);
function useThis(x) { const y = g(); return this.v + x + y; }
print("this-after-call", useThis.call({ v: 1 }, 2), ({ v: 5, m: useThis }).m(0));

// The four call shapes and their return fix-ups.
const o = {
  v: 7,
  m(x) { return this.v + x; },
  t(x) { return this.m(x); },           // tail_call_method
  chain(x) { return this.m(x) + this.m(x + 1); },
};
function tail(n) { return n === 0 ? "bottom" : tail(n - 1); }   // tail_call
function plain(n) { return n === 0 ? 0 : 1 + plain(n - 1); }     // call
print("shapes", o.t(1), o.chain(1), tail(3000), plain(3000), (function () { return g(); })());
print("call0-3", g(), (function (a) { return a; })(1), (function (a, b) { return a + b; })(1, 2), (function (a, b, c) { return a + b + c; })(1, 2, 3));

// Exceptions across flat frames: catch in the right frame, finally at every
// level, a stack string that still lists the frames.
function thrower(n) { if (n === 0) throw new Error("deep"); return thrower(n - 1) + 1; }
try { thrower(300); } catch (e) { print("throw-through", e.message, e.stack.split("\n").length > 3); }
let unwound = 0;
function fin(n) { try { return n === 0 ? 0 : fin(n - 1) + 1; } finally { unwound++; } }
print("finally", fin(200), unwound);
function catcher(n) { try { return thrower(n); } catch (e) { return "caught@" + n; } }
print("catch-mid-chain", catcher(50));
function rethrow(n) { try { return thrower(n); } finally { unwound++; } }
try { rethrow(10); } catch (e) { print("rethrow", e.message, unwound); }
print("stack-frames", (function outer() { return (function inner() { return new Error("x").stack; })(); })().includes("inner"));

// Mapped arguments alias the parameter through the flat callee's return.
function mapped(a, b) { arguments[0] = 9; return a + b + g(); }
print("mapped-args", mapped(1, 2));
function many(a) { return arguments.length + g(); }
print("argc", many(1, 2, 3, 4, 5));

// Generator and async floors calling flat children, with a yield / await
// between two of the calls.
function* gen() { const x = g(); yield x; yield g() + 1; return plain(3); }
const it = gen();
print("gen-flat-child", it.next().value, it.next().value, it.next().value);
async function af() { const v = g(); await 0; return v + g() + plain(2); }
af().then((v) => print("async-flat-child", v));
async function* ag() { yield g(); await 0; yield tail(10); }
(async () => { const out = []; for await (const v of ag()) out.push(v); print("asyncgen-flat-child", out.join()); })();

// Paths that stay C-recursive, each crossing back into a flat call.
print("apply", g.apply(null, []), Reflect.apply(o.m, { v: 1 }, [1]), plain.apply(null, [5]));
print("proxy", new Proxy(g, { apply(t, th, args) { return t() + 1; } })());
print("bound", o.m.bind({ v: 100 })(1));
print("getter", ({ get p() { return g(); } }).p);
print("sort", [3, 1, 2].sort((a, b) => plain(1) * (a - b)).join());
print("eval", (function () { return eval("g() + plain(1)"); })(), (0, eval)("g()"));
print("new", new (function K() { this.x = g(); })().x);
print("closure", (function () { let c = 0; const inc = () => ++c; inc(); inc(); return c; })());
