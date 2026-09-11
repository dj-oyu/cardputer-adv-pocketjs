// Built-ins calling back into JS: the native-frame-between-JS-frames case the
// spec says must stay "中断禁止" until audited. Results must be identical.
const log = [];

// sort with a comparator: stability, call count pattern is implementation
// detail so only the result is printed.
const people = [];
for (let i = 0; i < 60; i++) people.push({ k: (i * 7) % 5, i });
people.sort((a, b) => a.k - b.k);
print("stable", people.map((p) => p.k + ":" + p.i).slice(0, 14).join(","));
const nums = [5, 1, 4, 2, 3, 10, 21, 100];
print("default-sort", nums.slice().sort().join(), "numeric", nums.slice().sort((a, b) => a - b).join());

// Comparator throwing mid-sort: exception propagates, array stays a permutation.
const arr = [3, 1, 2, 5, 4];
let calls = 0;
try { arr.sort((a, b) => { if (++calls === 3) throw new Error("cmp"); return a - b; }); }
catch (e) { print("cmp-throw", e.message, arr.slice().sort().join()); }

// Comparator mutating the array it sorts: must not crash; result is a
// permutation of some values (only length is portable).
const mut = [4, 3, 2, 1];
mut.sort((a, b) => { mut.length = 4; return a - b; });
print("cmp-mutate", mut.length);

// Comparator that recurses into another sort.
const outer = [[3, 1, 2], [9, 7, 8], [6, 5, 4]];
outer.sort((a, b) => a.slice().sort()[0] - b.slice().sort()[0]);
print("nested-sort", JSON.stringify(outer));

// Iteration callbacks.
const src = [1, 2, 3, 4, 5, 6];
print("map", src.map((x, i, a) => x * i + a.length).join());
print("filter", src.filter((x) => x % 2).join());
print("reduce", src.reduce((acc, x) => acc * 10 + x, 0), src.reduceRight((acc, x) => acc + x, ""));
print("find", src.find((x) => x > 3), src.findIndex((x) => x > 3), src.findLast((x) => x < 3), src.findLastIndex((x) => x < 3));
print("every/some", src.every((x) => x > 0), src.some((x) => x > 5));
let fe = 0; src.forEach(function (x) { fe += x * this.m; }, { m: 3 });
print("forEach-this", fe);
print("flatMap", src.flatMap((x) => (x % 2 ? [x, x] : [])).join());
print("from-map", Array.from({ length: 4 }, (_, i) => i * i).join(), Array.from(new Set([1, 1, 2]), (x) => x + 1).join());

// Callback that mutates during iteration (spec: length captured at start).
const grow = [1, 2, 3];
const seen = [];
grow.forEach((x) => { seen.push(x); if (grow.length < 6) grow.push(x * 10); });
print("mutate-forEach", seen.join(), grow.join());

// JSON reviver / replacer / toJSON.
const parsed = JSON.parse('{"a":1,"b":{"c":[1,2,{"d":3}]}}', function (k, v) {
  log.push(k === "" ? "<root>" : k);
  return typeof v === "number" ? v * 100 : v;
});
print("reviver", JSON.stringify(parsed), log.join(",")); log.length = 0;
print("replacer", JSON.stringify({ a: 1, b: "x", c: [1, { d: 2 }] }, (k, v) => (typeof v === "number" ? v + 1 : v)));
print("toJSON", JSON.stringify({ when: { toJSON(key) { return "key=" + key; } } }), JSON.stringify({ a: 1, b: 2, c: 3 }, ["c", "a"], 1));

// String.replace with a function, RegExp callbacks, Symbol.replace override.
print("replace-fn", "a1b22c333".replace(/\d+/g, (m, off) => `<${m.length}@${off}>`));
print("replaceAll", "x.y.z".replaceAll(".", () => "-"));
const custom = { [Symbol.replace](s, r) { return "custom(" + s + "," + r + ")"; } };
print("symbol-replace", "abc".replace(custom, "R"));
class MyRe extends RegExp { exec(s) { log.push("exec"); return super.exec(s); } }
print("subclass-exec", new MyRe("b").test("abc"), log.join()); log.length = 0;

// Map/Set forEach, Object.groupBy / Map.groupBy if present, Array.prototype.join calling toString.
const m = new Map([["x", 1], ["y", 2]]);
let ms = ""; m.forEach((v, k, map) => { ms += k + v + map.size; });
print("map-forEach", ms);
const ts = { toString() { return "T"; } };
print("join-toString", [ts, 1, ts].join("-"));
if (typeof Object.groupBy === "function") {
  const grouped = Object.groupBy([1, 2, 3, 4, 5], (x) => (x % 2 ? "odd" : "even"));
  print("groupBy", JSON.stringify(grouped));
}

// Promise executor and then-callbacks are builtin->JS too; executor runs synchronously.
let execOrder = "";
new Promise((res) => { execOrder += "exec;"; res(); });
execOrder += "after;";
print("executor", execOrder);

// Getter on an array index read by a builtin.
const holey = [1, , 3];
Object.defineProperty(Array.prototype, 1, { get() { return "proto-getter"; }, configurable: true });
print("proto-index", holey.join(), holey.includes("proto-getter"));
delete Array.prototype[1];

// Callback throws out of a builtin: the builtin's partial work is visible.
const partial = [];
try { [1, 2, 3, 4].forEach((x) => { if (x === 3) throw new Error("stop@3"); partial.push(x); }); }
catch (e) { print("partial", partial.join(), e.message); }

// TypedArray sort with comparator, and string localeCompare-free sort.
const ta = new Int16Array([30, -5, 12, 0]);
print("typed-sort", Array.from(ta.sort((a, b) => b - a)).join());
