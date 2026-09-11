// Benchmark: closure creation and captured-variable access (JSVarRef paths).
function make(k) { let c = k; return () => (c = (c * 1103515245 + 12345) & 0x7fffffff); }
let acc = 0;
for (let i = 0; i < 20000; i++) {
  const f = make(i);
  for (let j = 0; j < 20; j++) acc = (acc + f()) & 0xffffff;
}
print("bench_closure", acc);
