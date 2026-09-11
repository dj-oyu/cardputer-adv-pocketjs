// Benchmark: tight arithmetic loop, no calls (backward-branch checkpoint cost).
let s = 0;
for (let i = 0; i < 3000000; i++) s = (s + (i ^ (i >>> 3))) % 1000000007;
print("bench_loop", s);
