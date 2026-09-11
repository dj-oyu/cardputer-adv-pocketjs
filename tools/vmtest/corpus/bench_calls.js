// Benchmark: JS->JS call/return (fib), the path L2b turns into explicit frames.
function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
print("bench_calls", fib(30));
