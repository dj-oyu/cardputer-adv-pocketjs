// try/finally unwinding through calls. Spec rule 6: a yield is not an
// exception -- finally must run exactly once, only on real completion.
const log = [];
const L = (s) => log.push(s);

function a() { try { L("a-try"); b(); L("a-unreached"); } finally { L("a-finally"); } }
function b() { try { L("b-try"); c(); } catch (e) { L("b-catch " + e.message); throw new Error("rethrown"); } finally { L("b-finally"); } }
function c() { try { throw new Error("from-c"); } finally { L("c-finally"); } }
try { a(); } catch (e) { L("top " + e.message); }
print(log.join(" | ")); log.length = 0;

// finally overrides: return in finally wins over throw and over return.
function overrideThrow() { try { throw new Error("lost"); } finally { return "finally-return"; } }
function overrideReturn() { try { return "try"; } finally { return "finally"; } }
function keepReturn() { let x = "try"; try { return x; } finally { x = "changed"; } }
print("override", overrideThrow(), overrideReturn(), keepReturn());

// break/continue through finally, labeled.
let s = "";
outer: for (let i = 0; i < 3; i++) {
  for (let j = 0; j < 3; j++) {
    try {
      if (j === 1) continue;
      if (i === 2) break outer;
      s += `${i}${j} `;
    } finally { s += "f "; }
  }
}
print("loop-finally", s.trim());

// Nested finally order and exception replacement.
function nested() {
  try {
    try { throw new Error("inner"); }
    finally { L("inner-finally"); }
  } catch (e) { L("caught " + e.message); throw new Error("replaced"); }
  finally { L("outer-finally"); }
}
try { nested(); } catch (e) { L("final " + e.message); }
print(log.join(" | ")); log.length = 0;

// Throw from finally replaces the pending exception.
try { try { throw new Error("first"); } finally { throw new Error("second"); } }
catch (e) { print("finally-throw", e.message); }

// Exception carrying through a getter, a constructor and a callback.
class K { constructor() { throw new TypeError("ctor"); } }
const objGet = { get g() { return new K(); } };
try { [1].forEach(() => objGet.g); } catch (e) { print("through-native", e.constructor.name, e.message); }

// Stack trace text (line:col with a basename label) is part of the baseline.
function s1() { return s2(); }
function s2() { return new Error("trace").stack; }
print(s1().trim().split("\n").map((x) => x.trim()).join(" / "));

// Non-Error throw values and finally with async.
try { throw { code: 7 }; } catch ({ code }) { print("destructured-catch", code); }
try { throw undefined; } catch (e) { print("throw-undefined", e); }
(async () => {
  try { await Promise.reject(new Error("async-e")); }
  finally { print("async-finally"); }
})().catch((e) => print("async-caught", e.message));

// Optional catch binding and completion values via eval.
print("completion", eval("try { 1 } finally { 2 }"), eval("L: try { 3; break L; } finally { 4 }"));
try { null.x; } catch { print("optional-catch"); }
