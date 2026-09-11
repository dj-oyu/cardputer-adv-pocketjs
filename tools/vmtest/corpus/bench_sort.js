// Benchmark: builtin->JS callback reentry (sort comparator, map, reduce).
let x = 12345;
const a = [];
for (let i = 0; i < 40000; i++) { x = (x * 1103515245 + 12345) & 0x7fffffff; a.push(x % 100000); }
a.sort((p, q) => p - q);
const m = a.map((v, i) => v ^ i).reduce((s, v) => (s + v) % 1000003, 0);
print("bench_sort", a[0], a[a.length - 1], m);
