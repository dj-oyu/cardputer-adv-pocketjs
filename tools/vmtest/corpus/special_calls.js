// Every call kind the spec (L2 "フレームの保持内容") says must be classified:
// getter/setter, Proxy, eval, bound, constructor, apply/call, tagged
// templates, coercion callbacks.
const log = [];
const L = (s) => log.push(s);

// getters/setters, including inherited and defineProperty ones.
const base = { get v() { L("base-get"); return this._v * 2; }, set v(x) { L("base-set " + x); this._v = x; } };
const child = Object.create(base);
child.v = 4;
L("child.v=" + child.v);
Object.defineProperty(child, "w", { get() { return "w-" + this.v; }, configurable: true });
L(child.w);
print(log.join(" | ")); log.length = 0;

// Proxy traps, including a trap that is itself a Proxy.
const target = { a: 1, f(x) { return x + this.a; } };
const handler = {
  get(t, k, r) { L("get " + String(k)); return Reflect.get(t, k, r); },
  set(t, k, v, r) { L("set " + String(k)); return Reflect.set(t, k, v, r); },
  has(t, k) { L("has " + String(k)); return k in t; },
  ownKeys(t) { L("ownKeys"); return Reflect.ownKeys(t); },
  deleteProperty(t, k) { L("delete " + String(k)); return delete t[k]; },
};
const px = new Proxy(target, handler);
px.a = 5;
L("f=" + px.f(1));
L("in=" + ("a" in px));
L("keys=" + Object.keys(px).join());
delete px.a;
print(log.join(" | ")); log.length = 0;

const fnProxy = new Proxy(function (x) { return "called " + x; }, {
  apply(t, thisArg, args) { L("apply"); return t(...args) + "!"; },
  construct(t, args, nt) { L("construct " + (nt === fnProxy)); return { made: args[0] }; },
});
L(fnProxy("p"));
L(JSON.stringify(new fnProxy("q")));
const metaProxy = new Proxy({}, new Proxy({}, { get(t, trap) { L("meta " + trap); return undefined; } }));
metaProxy.x;
print(log.join(" | ")); log.length = 0;

// Revoked proxy.
const { proxy: rp, revoke } = Proxy.revocable({}, {});
revoke();
try { rp.x; } catch (e) { print("revoked", e.constructor.name); }

// eval: direct sees locals, indirect sees globals, strict eval has own scope.
var gv = "global";
function evals() {
  var gv = "local";
  const direct = eval("gv");
  const indirect = (0, eval)("gv");
  eval("var introduced = 'intro'");
  const strictEval = (function () { "use strict"; eval("var hidden = 1"); return typeof hidden; })();
  return [direct, indirect, introduced, strictEval].join(",");
}
print("eval", evals());
print("eval-new-function", new Function("a", "b", "return a * b")(6, 7));
print("eval-nested", eval("eval('1 + eval(\"2\")')"));

// bound functions: partial args, this, new ignores bound this, name/length.
function who(a, b, c) { return [this && this.id, a, b, c].join("/"); }
const bound = who.bind({ id: "B" }, 1);
const bound2 = bound.bind({ id: "ignored" }, 2);
print("bound", bound(9, 8), bound2(3), bound.name, bound.length, bound2.length);
function Pt(x, y) { this.x = x; this.y = y; }
const BPt = Pt.bind(null, 10);
const bp = new BPt(20);
print("bound-new", bp.x, bp.y, bp instanceof Pt, bp instanceof BPt);

// constructors: new.target, class hierarchy, super, returning objects.
class A {
  constructor(tag) { this.tag = tag; this.nt = new.target.name; }
  hello() { return "A:" + this.tag; }
  static make() { return new this("static"); }
}
class B extends A {
  constructor() { super("from-B"); this.extra = true; }
  hello() { return "B>" + super.hello(); }
}
const bi = new B();
print("class", bi.hello(), bi.nt, bi.extra, B.make().hello(), B.make() instanceof B);
function Ret() { return { replaced: true }; }
function RetPrim() { this.kept = true; return 5; }
print("ctor-return", JSON.stringify(new Ret()), JSON.stringify(new RetPrim()));
class D extends A { constructor() { return { notThis: 1 }; } }
print("derived-return-object", JSON.stringify(new D()));
class E extends A { constructor() { } }
try { new E(); } catch (e) { print("derived-no-super", e.constructor.name); }
try { A(); } catch (e) { print("class-call", e.constructor.name); }
print("reflect-construct", Reflect.construct(A, ["rc"], B).nt);

// call/apply/Reflect.apply with array-likes, spread with many args.
const many = Array.from({ length: 300 }, (_, i) => i);
print("apply", Math.max.apply(null, many), who.call({ id: "C" }, "x"), Reflect.apply(who, { id: "R" }, ["y", "z"]));
print("spread", Math.min(...many, -1), ((...r) => r.length)(...many, ...many));

// tagged templates share a frozen strings object per call site.
function tag(strs, ...vals) { return strs.raw.join("|") + "#" + vals.join() + "#" + Object.isFrozen(strs); }
const site = () => tag`a${1}b${2}\n`;
print("tagged", site(), (() => { const x = []; for (let i = 0; i < 2; i++) x.push(((s) => s)`k`); return x[0] === x[1]; })());

// coercion callbacks: valueOf/toString/Symbol.toPrimitive invoked by operators.
const coerce = {
  valueOf() { L("valueOf"); return 3; },
  toString() { L("toString"); return "S"; },
};
L("sum=" + (coerce + 1));
L("str=" + `${coerce}`);
L("cmp=" + (coerce > 2));
const prim = { [Symbol.toPrimitive](hint) { L("hint " + hint); return hint === "number" ? 42 : "p"; } };
L("prim=" + (+prim) + "," + `${prim}` + "," + (prim + ""));
print(log.join(" | ")); log.length = 0;

// Getter throwing inside a deep call path.
const thrower = { get boom() { throw new RangeError("getter-boom"); } };
function pathA() { return pathB(); }
function pathB() { return thrower.boom; }
try { pathA(); } catch (e) { print("getter-throw", e.constructor.name, e.message); }

// Function.prototype.toString for a user function is source text.
print("toString", (function named(a) { return a; }).toString());
