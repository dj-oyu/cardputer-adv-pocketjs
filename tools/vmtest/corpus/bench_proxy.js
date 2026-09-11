// Benchmark: Proxy get/set traps and getter/setter calls (native->JS->native).
const t = { v: 0 };
const p = new Proxy(t, { get: (o, k) => o[k], set: (o, k, v) => ((o[k] = v), true) });
const g = { _x: 0, get x() { return this._x; }, set x(v) { this._x = v; } };
for (let i = 0; i < 200000; i++) { p.v = (p.v + i) % 1000003; g.x = (g.x + 1) & 0xffff; }
print("bench_proxy", t.v, g.x);
