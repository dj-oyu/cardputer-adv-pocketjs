# Prepared startup measurement (PR48)

This benchmark branches from PR48 `879da5aaa818c497d29cd6c3b67d9134cd315abe`.
It does not change product source. It is a standalone registration experiment,
not DERBY FPS, display throughput, or the normal GUI firmware. DERBY's dynamic
`H.register('derby.' + p[0], p[1], p[2])` remains generic.

## Host

```
python3 tools/vmtest/prepare/bench/run.py --pairs 1000
python3 tools/vmtest/prepare/bench/run.py --m32 --pairs 1000
```

Outputs are unique `.cache/prepared-bench/{native,m32}/<UTC timestamp>/trials.jsonl`
and `summary.json`, with source/fixture hashes, compiler/platform and affinity.
The script pins itself to one allowed CPU on Linux. Shared-runner scheduling
and cache contention remain; distributions are observations, not acceptance
thresholds. Run several sessions and preserve every raw result, including
outliers. ASan tests correctness separately, never production speed.

Every trial has a new runtime/context and isolated native startup state.
Only `next_handle` is reset explicitly by the isolated harness; the normal
adapter intentionally preserves its monotonic handle counter across resets.
Both modes receive exactly the same fixture bytes and generated real DERBY
ROM table, filename, setup and optimizer. Setup never materializes `register`.
Validation after the interval requires handle 1, the real ROM plan with one
argument equal to 512, exactly one prepared hit for supported/prepared,
zero hits otherwise, zero misses and no leaked active descriptor. Allocator
accounting additionally requires both tracked domains empty after teardown.
Eight warmup pairs precede each case/pass; recorded pairs alternate G/P and
P/G. These are **fresh-context startup on a warmed process/cache** trials,
not cold-cache or whole-boot latency. Setup/teardown are excluded.

The supported source is read from the existing runtime-startup.js fixture;
it is not copied into a different JS microbenchmark. The unsupported source
wraps the same fixture in a runtime conditional, forcing the existing
branch-rejection path while preserving the same result. Generic/prepared
comparisons always use identical source within each case.

Timing covers the complete loader: compilation, recognition, descriptor
scope, VM dispatch and native registration. Timing uses normal engine/native
allocators. Accounting is a separate pass using custom QuickJS and native
allocators. It reports allocation/reallocation requests, full new requested
bytes on realloc, peak live payload above the setup baseline, and retained
payload at the end of the interval. Header overhead, libc/TLSF rounding and
engine-private metadata are excluded: these are **requested payload** bytes,
not physical heap occupancy. The custom usable-size callback returns requested
size rather than libc usable capacity; engine allocation decisions can differ
from the ordinary allocator. Accounting results describe this instrumented
allocator, not an exact reconstruction of release allocator traffic. Native allocations made inside separately linked
procedural helpers are not intercepted; for this fixture the adapter's plan
is its only native allocation. Accounting wrappers change allocation layout
and cost; their timings are not performance results. POCKET_PREPARED_TEST
adds one hit/miss counter update to the prepared path for proof, so even the
timing build is a diagnostic rather than a byte-identical release binary.
Native timing wrappers forward to libc malloc/calloc/free, retaining a shared
mode-check branch; they do not substitute malloc+memset for ordinary calloc.
No log output or heap object walk occurs inside the measured interval.

## Cardputer ADV artifact and selector

The `Prepared runtime benchmark` workflow builds a dedicated ESP32-S3 image
using ESP-IDF 6.0.1, 240 MHz, no PSRAM/power management and O2. The main task
has a diagnostic 32 KiB stack, unlike the normal GUI's 8 KiB. It uses the real
engine, adapter and generated ROM registry, with presentation stubs; no LCD,
keyboard, network or normal app shell is initialized. It is a standalone
benchmark firmware app for the Cardputer ADV, not an installable GUI app.

The same binary automatically selects generic/prepared for 64 alternating
pairs per case/pass, with the same eight warmup pairs. There is no USB command
or mutable product toggle. The recorded `mode` is checked against actual hits.
Timing is `esp_timer_get_time()` in microseconds (reported as ns multiples of
1000); a one-call difference below that resolution is not established.
The counter/monitor instrumentation is symmetric, but both paths retain
PR48's engine hooks and code placement. This does not measure their cost
relative to the old integration. Product-size builds compare those versions
separately; an old/new runtime timing comparison would be additional work.

Device rows include internal 8-bit heap free before evaluation, local minimum
free during evaluation and largest block afterward. IDF sums per-region
low watermarks, which may occur at different times: this is not a simultaneous
whole-heap peak. The monitor is restarted
for each trial, read/stopped before validation/logging, and includes allocator
rounding and any concurrent system allocation. Requested guest/native peak
accounting remains separate. No JS_ComputeMemoryUsage heap walk occurs in the
interval. Scheduler yields occur only after teardown. Serial output happens
between trials and can affect later cache/scheduling state; repeat sessions
and reverse ordering in a follow-up if a small difference is observed.

CI artifact `cardputer-prepared-bench-<head>` contains the image, bootloader,
partition table, flasher_args.json, ELF/map, sdkconfig, source-head and
SHA256SUMS. The artifact is build-verified, **not hardware-tested**. This task
does not access USB, flash or alter a device. An authorized operator can:

1. Check the artifact's exact source head and `sha256sum -c SHA256SUMS`.
2. Treat it as a standalone image with its own partition table; preserve the
   existing firmware/data using the established device procedure first.
3. If separately authorized, install using the artifact's flasher_args.json
   and capture a complete fresh-boot serial log through `{"complete":true}`
   and `PREPARED_BENCH_DONE`. Keep board/clock/image hash with the log.
4. Parse only JSON lines with
   `python3 tools/vmtest/prepare/bench/run.py --summarize capture.log`.
   Missing completion, rows, hits or plan-result assertions invalidate a run.

## Product size

The workflow also links the exact old integration `578d552` and PR48 `879da5a`
with the same SDK, original default configuration and fixed PROJECT_VER.
Artifacts `prepared-product-size-<source>-<bench-head>` retain ELF, map,
sdkconfig, image, size/size-files reports and hashes. These are original
product images, separate from the benchmark image; do not compare the small
benchmark image to the GUI product. Compare actual sdkconfig and compiler
options before attributing flash text/rodata or static RAM deltas to PR48.
No source file in either pinned revision is rewritten by the benchmark.

## First local observation (provisional)

The initial unpinned native O2 run, 200 pairs per case/pass, measured supported
loader median 5.892 us generic versus 6.2995 us prepared (paired median ratio
1.0603). P95 was 11.837/13.115 us and outliers reached 98/621 us, so this is
not a stable speedup estimate. Both modes had 50 QuickJS requests, 2,945 B
cumulative payload and 1,355 B peak increment; native was one 56 B request
and peak. Fallback paired median ratio was 1.00395 with broad variation.
This initial timing pass used a calloc wrapper implemented as malloc+memset;
independent review caught and corrected that distortion. Its timing values
are retained only as development provenance and are superseded by corrected
runs. These observations establish no speed or heap improvement. The host had
no usable i386 package set; m32 and target builds are delegated to CI.
A subsequent pinned 1,000-pair run and exact-head CI results should be used
with their raw evidence rather than replacing this provenance silently.
