// Math fast call (js_call_math_fast, docs/vm/math-call-fast.md): the call
// opcodes run a pure Math builtin on number arguments without a native frame.
// Every line here must print what the generic call prints; expected/ was
// written by the engine WITHOUT the fast path. The guard is the callee value,
// so the cases are: values (-0, NaN, ints), the call forms that reach it,
// replacements that must not, and arguments that must take the generic path.
const show = (v) => (Object.is(v, -0) ? "-0" : String(v));
const row = (name, f, args) => print(name, args.map(show).join(","), "->", show(f(...args)));

// 1. Values, through the plain call form (OP_call*, the `const sin = M.sin` alias DERBY uses).
const M = Math, sin = M.sin, cos = M.cos, sqrt = M.sqrt, flo = M.floor, rnd = M.round;
const mn = M.min, mx = M.max, pow = M.pow, imul = M.imul, atan2 = M.atan2, abs = M.abs, sign = M.sign;
const V = [0, -0, 1, -1, 0.5, -0.5, 2.5, -2.5, 1e300, -1e-300, NaN, Infinity, -Infinity, 2147483647, -2147483648];
for (const x of V) print("v", show(x), show(sin(x)), show(cos(x)), show(sqrt(x)), show(flo(x)),
  show(rnd(x)), show(abs(x)), show(sign(x)));
for (const [a, b] of [[0, -0], [-0, 0], [1, NaN], [NaN, 1], [3, 2], [2.5, 2], [-1, -1.5], [Infinity, -Infinity]])
  print("mm", show(a), show(b), show(mn(a, b)), show(mx(a, b)), show(pow(a, b)), show(atan2(a, b)));
for (const [a, b] of [[3, 5], [-3, 5], [0x7fffffff, 2], [0xffffffff, 5], [2 ** 32 + 5, 3], [1.9, 2.9], [-1.5, 7], [NaN, 3], [Infinity, 2]])
  print("imul", show(a), show(b), show(imul(a, b)));
print("int-result", show(flo(3.7)), show(rnd(-0.4)), show(pow(2, 10)), show(mx(1, 2.5)), show(mn(1, 2)));

// 2. The other forms: method call (OP_call_method, `M.sin(x)` / `Math.sin(x)`),
// a local alias, a property of another object, a tail call.
function local(x) { const s = Math.sin; return s(x) + Math.cos(x) + M.imul(x | 0, 3); }
const holder = { f: Math.sqrt, g: Math.max };
function tail(x) { return Math.pow(x, 3); }
print("forms", show(local(0.25)), show(holder.f(16)), show(holder.g(1, 7, 3)), show(tail(-2)));

// 3. Arity: fewer than length takes the generic path (undefined -> NaN), more
// than length passes the extras through unconverted.
let conv = 0;
const spy = { valueOf() { conv++; return 1; } };
print("arity", show(Math.sin()), show(Math.pow(2)), show(Math.min()), show(Math.max()), show(Math.min(4)),
  show(Math.imul(3)), show(Math.atan2(1)), show(Math.max(1, 2, 3, 9, 4)));
print("extra-not-converted", show(Math.sin(0, spy)), show(Math.pow(2, 3, spy)), conv);

// 4. Non-number arguments: conversion runs, in order, with its side effects.
const order = [];
const o = (tag, v) => ({ valueOf() { order.push(tag); return v; } });
print("conv", show(Math.sin(o("a", 0))), show(Math.max(o("b", 1), o("c", 5))), show(Math.imul(o("d", 6), 7)),
  show(Math.pow(o("e", 2), o("f", 3))), order.join(""));
print("coerce", show(Math.sqrt("16")), show(Math.floor(true)), show(Math.abs(null)), show(Math.sin(undefined)),
  show(Math.min("3", 2)), show(Math.imul("4", 4.9)));

// 5. Throwing conversions: the exception and its backtrace come from the
// generic call, native frame included.
for (const [name, f] of [["sin-bigint", () => Math.sin(1n)], ["max-throw", () => Math.max(1, { valueOf() { throw new Error("vo"); } })],
                         ["imul-symbol", () => Math.imul(Symbol(), 1)]]) {
  try { f(); print(name, "no throw"); }
  catch (e) { print(name, e.name, e.message, e.stack.split("\n").slice(0, 2).map((l) => l.trim()).join(" | ")); }
}

// 6. Replacements: the call site sees whatever function value it loads.
function site(x) { return Math.sin(x); }
const keep = Math.sin;
print("before", show(site(0)));
Math.sin = (x) => "replaced:" + x;
print("replaced", site(1));
Math.sin = keep;
print("restored", show(site(0)));
const realMath = globalThis.Math;
globalThis.Math = { sin: (x) => "fake:" + x };
print("reassigned", site(2));
globalThis.Math = realMath;
Object.defineProperty(Math, "cos", { get() { order.push("get"); return keep; }, configurable: true });
print("getter", show(Math.cos(0)), order[order.length - 1]);
Object.defineProperty(Math, "cos", { value: cos, writable: true, configurable: true });
print("bound", show(Math.sqrt.bind(null)(9)), show(Math.max.bind(null, 4)(2)));
print("proxy", show(new Proxy(Math.abs, {})(-3)), show(Reflect.apply(Math.min, null, [5, 1])), show(Math.sin.call({}, 0)));
try { new Math.sin(1); } catch (e) { print("construct", e.name); }

// 7. Same site, changing callee, in a loop (no caching anywhere).
const fs = [Math.sin, Math.abs, (x) => x * 10, Math.floor, Math.max];
let acc = "";
for (let i = 0; i < 10; i++) acc += show(fs[i % fs.length](-1.5)) + " ";
print("loop", acc.trim());
