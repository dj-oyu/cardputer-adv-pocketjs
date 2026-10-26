// get_var -> get_var_lex: reads of top-level let/const/class rewrite themselves
// to a slot read the first time they find an initialized binding. Each line
// checks itself; the output must not change with or without the rewrite.
function check(name, got, want) { print(name, got === want ? "ok" : "NG got=" + String(got) + " want=" + String(want)); }

// Read in the TDZ: must throw, and must not be rewritten (still throws later
// if called before init; here it is called once before and then after).
function early() { return lateLet; }
let tdz;
try { early(); tdz = "no throw"; } catch (e) { tdz = e instanceof ReferenceError; }
check("tdz throws", tdz, true);
let lateLet = 7;
check("after init", early(), 7);

// Live value: the slot is read each time, not cached.
let counter = 0;
function readCounter() { return counter; }
let sum = 0;
for (let i = 0; i < 1000; i++) { counter = i; sum += readCounter(); }
check("live reads", sum, 499500);

// const, class, typeof (get_var_undef) on a lexical binding.
const K = 41;
class Point { constructor(x) { this.x = x; } }
function useConst() { return K + 1; }
function useClass() { return new Point(3).x; }
function typeofLex() { return typeof K; }
for (let i = 0; i < 3; i++) { useConst(); useClass(); typeofLex(); }
check("const", useConst(), 42);
check("class", useClass(), 3);
check("typeof lexical", typeofLex(), "number");

// Global object properties are not lexical: never rewritten, still live,
// including after a delete.
globalThis.gp = 1;
function readGp() { return typeof gp === "undefined" ? "gone" : gp; }
check("global object", readGp(), 1);
globalThis.gp = 2;
check("global object live", readGp(), 2);
delete globalThis.gp;
check("global object deleted", readGp(), "gone");
check("builtin", (function () { return Math.max(1, 2); })(), 2);

// Many functions reading many bindings, in an order that interleaves sites.
let a0 = 1, a1 = 2, a2 = 3, a3 = 4;
function mix(n) { let t = 0; for (let i = 0; i < n; i++) t += (i & 1 ? a0 + a2 : a1 + a3); return t; }
check("mix", mix(10), 50);
a2 = 30;
check("mix after store", mix(10), 185);

// Closures over rewritten code are unaffected.
const reader = (() => () => lateLet + K)();
check("closure", reader(), 48);
