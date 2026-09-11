// Benchmark: allocation churn with cyclic garbage (cycle GC runs under --profile host).
let live = [];
let s = 0;
for (let i = 0; i < 60000; i++) {
  const o = { i, next: null, arr: [i, i + 1] };
  o.next = o;
  live.push(o);
  if (live.length > 64) live = [];
  s = (s + o.arr[1]) % 1000003;
}
print("bench_alloc", s);
