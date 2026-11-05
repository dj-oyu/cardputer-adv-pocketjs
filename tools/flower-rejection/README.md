# Per-row span narrowing for ray_row (rejected experiment)

The write-up (closed form, bit-exactness result, why it lost on device and the
0.2 ms arithmetic) moved to `docs/perf/flower-perf.md`, section "ray_row の行ごとの
span 狭め". The harness (`measure_visits.c`, `test_bitexact.c`) and the fragment
(`rejection.c.frag`) stay here.
