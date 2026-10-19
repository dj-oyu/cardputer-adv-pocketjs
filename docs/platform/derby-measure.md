# DERBY stage accounting experiment

Baseline: local `970a3b5f3d02aa1b8311b186d99a3842499ec56a`, tree
`11ab870bb53b811da89c771f247c390e23c092b4` (public PR8 `f7d86c1`, normal OFF).
The installed diagnostic `9db14c88` is a different baseline. Do not combine
its 22–24 fps windows with these counters or describe host times as device times.

`POCKET_DERBY_MEASURE=OFF` is the default. The sources are absent from the
component and scope/backend macros compile to no code, even at O0. Enabling
it does not alter guest JS, racecore, clocks, seed, renderer inputs or output.
This patch does not optimize anything.

## Boundaries

Each `sN` is `inclusive_us:self_us:calls:max_inclusive_us:max_self_us`.
Maxima are reset-to-date maxima, never interval maxima obtained by subtraction.
Owner-task stack depth is 24; every C return (also errors) closes its scope.
A child subtracts its entire elapsed time from its parent once; repeated
same-stage recursion therefore also has correct aggregate self time.
Never add guest inclusive time to bridge/native/VM time.

| N | scope | coverage |
|---|---|---|
| 0 | guest | frame/continuation call, including nested native and job work; no parked interval |
| 1 | bridge | DERBY course/view/ser; procedural register/unregister/begin/draw/commit |
| 2 | native | view/ser calculation after validation; native draw_numbers validation/conversion envelope |
| 3 | procedural build | draw_run including VM startup, bounds, segment copying, typed points |
| 4 | VM | three plan-run APIs, includes scalar interpreter/fused execution |
| 5 | points | affine dispatcher including selection and scalar/PIE transform |
| 6 | raster | render_rects including backdrop/compositing; child synchronous sends excluded from self |
| 7 | transfer | board_present[_rect]_sync, including reap/wait; excludes text/pet overlay |
| 8 | output | bounded USB write; previous outcome appears in next record |
| 9 | observer | heap checkpoint reading, serialization/output inclusive; excludes clock-hook cost |

Guest self is **residual**, not pure JS: other uninstrumented APIs, interrupts,
OS scheduling and timer overhead remain there. Native self also contains timer
cost and cannot be labeled pure arithmetic. Wall-clock scopes measure elapsed
time, not CPU cycles. Timer pair calibration min/max (32 pairs) is reported;
it is not the complete hook overhead and is never subtracted from fps.
No instrumentation of individual VM instructions, pixels or horses occurs.

Board synchronous APIs reap before and after their own submit. Transfer timing
therefore includes blocking completion wait but does not separately identify
submit and ISR/DMA duration, and proves neither physical LCD scan-out nor image
coherence. Transfer self is already inside raster inclusive; do not add it to
frame elapsed. Overlay work remains raster self.

## Bounded output and interpretation

`DERBY_MEASURE v=1` records are cumulative from app evaluation, at most one
attempt per device second across app resets and 300 attempts per app, without retries or guest
allocation. A fixed 1024-byte local buffer and 536-byte snapshot are used (~1.6 KiB additional peak task stack). A snapshot that cannot fit the USB 1024-byte contract emits an explicit `overflow=1` invalid record and increments the refusal counter; it is never truncated into an apparently valid record. Counter/scope storage is fixed (~1.1 KiB);
64-bit totals are adequate for the bounded experiment, not a perpetual service.
`us` is monotonic sampling wall time; use differences for rates. Final partial
interval and last output outcome are absent. Any nonzero `faults`, `refused`,
dispatch failure or nonconsecutive seq invalidates a controlled comparison.

`ctx` counts new guest frame dispatches, `cont` continuation dispatches, and
`done` counts each latest context once when work/procedural queues are empty
and some bands succeeded. This is a conservative work/presentation checkpoint,
not physical LCD-frame count: never divide one by another without checking
context/phase/camera stability. Frames with no damage are not completed here.
Transfer success/failure and successful bytes are counted per synchronous call.
`capture` counts sampled presentations and transfer entries with capture enabled: such intervals
are disturbed and must not be pooled with ordinary performance.
`scalar/pie` counts successful typed-point batches, not horses: PIE is the same
transform over at least eight vertices. Heap max is a **sampled lower bound**;
cap is the most recent checkpoint cap, not minimum headroom.

Counters do not contain camera/seed/tier. For target performance, pair the
monotonic record interval with the existing diagnostic context evidence and
reject scene/camera/tier changes, gaps, capture, failures and ambiguous identity.
Do not infer stable context merely from equal endpoints. If that evidence is
insufficient, add bounded context transitions in a separate measurement change
before claiming per-camera timing; aggregate mixed-camera counters do not prove it.

## Host acceptance and next target experiment

`python3 tools/test_derby_measure.py` checks deterministic exclusive accounting,
early return, same-stage recursion, overflow/clock faults, counters, and O0
compile-out. `python3 tools/games/run_derby.py --measure --heap-limit 524288`
executes the actual QuickJS/native/renderer functional oracle with counters.
Host oracle/debug redraws are included, so its counts and timings are **functional
coverage only**, unsuitable for production hot-path ranking. The test preserves
pixel, seed, finish and ordered draw-input assertions. The existing 100094 lane
frames and 2923 PAN frames run first.

Before device optimization decisions: use identical seed/step/camera/tier and
quality, the same measuring code, exact-head CI/build receipts; compare OFF and
ON wall throughput and checkpoint heap without fps correction, then compare one
optimization with ON unchanged. Preserve the last successful artifact/evidence.
No USB operation, remote build, flash or device input is performed by these tests.

CI builds three exact-head target configurations: normal OFF/OFF, telemetry
ON/OFF, and measurement ON/ON (telemetry/measurement). Every receipt preserves
the existing source/tree, actual BIN/ELF and embedded-JS checks and additionally
checks the unique measurement CMake value against defined ELF symbols: no
`dm_*` in OFF, the exact public measurement symbol set in ON. Artifact names
include both modes, so the two telemetry-ON builds cannot overwrite one another.

## Explicit source diagnostic variant

This branch adds ON/ON source defaults, fixed diagnostic seed and mandatory ALL
output proof to PR11. The normal branch remains OFF/OFF with hardware seed.
None of the settings is FORCE; a stale or caller-supplied OFF cache fails the
proof. The proof checks actual generated JS,
telemetry and measurement defined ELF symbols, IDF 6.0.1 descriptor, and matching
BIN/ELF SHA256. It runs without Git and writes only under the build directory;
Python -B prevents pyc mutation. The broker's durable source receipt supplies
full commit/tree identity externally; bind the same successful job's proof
BIN/ELF hashes to its manifest, never just a local proof or mode label. Exact-head
CI independently verifies provenance and both ON expectations. This branch's CI
uses defaults (no mode overrides) and checks complete/incremental source
immutability; it does not weaken the diagnostic guarantee to run an OFF matrix.

The comparison source also defaults `POCKET_DERBY_COMPARE_FIXED_HW=ON`: its
native `pocket.random.seed()` returns `0x2545F491`, the host harness's existing
`DERBY_HW` default. The option is OFF in ordinary product builds and requires
telemetry and measurement when enabled. A normal build still calls
`esp_random()`; the CI test compiles both actual C paths. The build proof
rejects a stale OFF CMake cache and reads the fixed seed object returned by
the provider from the ELF, instead of trusting the cache alone.

Only the diagnostic image embeds a generated `derby_view.js` with `sn=19`
regardless of the asynchronously loaded saved race number. Its `save()` is a
no-op, so the comparison image cannot change the player's `derby.v1` NVS data;
it may still read the saved race and points for the paddock. The product JS,
storage behavior and race formula remain unchanged. The mandatory proof
requires the exact generated view bytes in the ELF and rejects the shipping
view bytes there. For demo race 1000001 the observed seed must be `9112A7AB`.

The first device comparison may use only continuous, gap-free camera windows
whose logged seed, tier, race, phase, camera and context progression agree.
Record each window's start/end sample and device time, camera and context
delta; reject capture, failures, missing telemetry and divergent conditions.
Camera-change logs do not identify the simulation step, so equal camera names
alone do not prove a whole-race identical trajectory. Mixed-camera aggregate
FPS is not an optimization claim.
