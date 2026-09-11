// Benchmark: generator resume/suspend and yield* delegation.
function* range(n) { for (let i = 0; i < n; i++) yield i; }
function* twice(n) { yield* range(n); yield* range(n); }
let s = 0;
for (const v of twice(200000)) s = (s + v) % 1000003;
print("bench_generator", s);
