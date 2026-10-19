# Finite fixed DERBY workload (diagnostic only)

`POCKET_DERBY_FIXED_WORKLOAD` defaults to OFF. The source images in these
branches already enable the older telemetry/measurement/fixed-seed modes; this
new workload is a separate opt-in. It requires measurement, fixed hardware input
and ordinary app chunks. Normal OFF keeps the original owner/app path and native
machine-code text; the fixture API, counter storage, clocks and generated fixture
bytes are omitted. No SIMD, scheduler policy, flash, recovery, authorization or
readback rule changes are part of this work.

## Exact work and boundaries

Fixture `oval-pan8-390-510-v1` uses hardware input `2545F491`, saved seed input
19, race 1000001, actual seed `9112A7AB`, explicit oval, tier 1, camera 8 and
interpolation 1. It renders exactly 120 contexts, advancing from step 390 through
510 one simulation step per context. No keys, director changes, slow motion,
capture or storage are allowed. All variants use the same generated watch/view/
play bytes; batch additionally uses its existing PAN submission transform.

Each replay drops old plans, recreates the course via `enter('pad')`, loads the
ordinary resources, passes the ordinary gate and renders fixed camera 8 to step
390. Course replacement invalidates the native view. The native cache is then
refreshed by the same view calls during warmup, exactly as in the original app.
The measured interval starts warm; the cache is not artificially flushed inside
it. All diagnostic counters are cleared at the start boundary, independently of
the cache and existing cumulative stage counters.

After the step-390 context has been presented, the next owner loop activates the
replay before any measured dispatch. Its `dm_read` snapshot is quiescent. The
120th context must report step 510 and finish presentation. The owner loop then
performs its unchanged normal pacing, takes a second quiescent snapshot and
closes the interval. A continuation is another owner turn, counted separately;
it never becomes another context. Missing/repaired work cannot silently create
another accepted context. Exactly 120 new and completed contexts are required.

After that interval, `settle()` completes the simulation without additional
rendered contexts. Its ordered finish times rounded to microseconds must hash to
`5DC86E18`. This tail, summary output and subsequent reset are unmeasured.
Three replays run in the same boot and share the process, allocator and thermal/
scheduling environment. They are **dependent repetitions, not independent trials**.
After the third successful summary the fixture exits. A 180-second session cap,
800-context warmup cap, three-replay cap and fixed buffers bound the operation.

## Direct elapsed-time accounting

`wall_us` brackets the complete measured replay from first owner-loop start
through the normal wait after its final completed context. `work_us` directly
sums each measured owner loop's `frame_start` to the point just before the
existing pace calculation. It includes input/pumps, guest, render, transfers,
other owner work and any scheduling preemption in that span. It excludes the
actual wait calls. Gaps between instrumentation hooks remain in `wall_us`;
they are not assigned to CPU or pace by subtraction.

`p0`, `p1`, `p2` directly bracket the continuation `vTaskDelay`, ordinary
minimum-period `vTaskDelay`, and frame-cap `vm_wake_wait` respectively. Each is
`elapsed_us:calls:requested_ticks`. For the cap wait, the existing source-wait
policy is evaluated once and its actual selected tick count is recorded. The
wait API does not expose why it returned: no wake reason is inferred from its
duration. The wait expression, floors, caps and notification behavior remain
unchanged. These and stage self times are **elapsed monotonic times, not CPU
times**. No clock calibration, residual or estimated overhead is subtracted.

`s0`..`s9` are cumulative snapshot deltas `self_us:calls` for guest, bridge,
native, procedural build, VM, points, raster, transfer, output and observer.
They have the same scope definitions as `derby-measure.md`. Child elapsed time
is removed from a parent's self once. Guest self is residual and includes
uninstrumented work, preemption and diagnostic calls. Never add guest inclusive
to its child stages. No interval maximum is obtained by subtracting maxima.
The existing per-present memory checkpoint remains in observer; periodic
telemetry and measurement writes are suppressed throughout this fixture.

`c0`..`c9` are course calls, view calls, ser calls, clip predicate tests, square
root evaluations, cache guard hits, cache guard misses, view cache refreshes,
runner batch calls and used batch rows. They count only inside the measured
interval. A view refresh is not a cache guard miss. Baseline and batch have no
clip cache; their square root count is one per clip predicate. The original
numeric expressions and draw order are retained. Counter overhead is present
and is not estimated away. Batch may move time/calls between bridge and native;
that movement itself is not a performance gain.

## Complete-trial quality and output

There is one `DERBY_FIXED` end summary per successful replay and one
`DERBY_FIXED_END` whole-trial record. Each write is bounded to 1024 bytes and is
attempted once. There is no per-frame output or capture. A failed load, input,
step/identity mismatch, missing context/presentation, capture, dispatch/render/
transfer failure, clock regression, counter overflow, output refusal or timeout
latches trial failure and stops. An invalid repetition is never omitted while
the remaining repetitions are accepted. The final record explicitly says
`independent=0`.

`tools/games/derby_fixed_result.py` requires all three consecutive complete
summaries plus the final valid record, identical fixture/manifest/session/
variant, exact context/step/finish identities, and complete transport supplied
by the owning collector. Partial tails, gaps, duplicate records, overflow and
any invalid record make the whole trial indeterminate. A complete line alone
does not establish the collector's transport condition. The existing MCP demo
collector does not yet implement this finite protocol; before any hardware run
it needs an explicit bounded collector and transport-quality contract. This
implementation does not add a hardware operation or bypass its approval rules.

## Host checks and adoption rule

Run `python3 -B tools/test_derby_fixed_workload.py` for deterministic controller,
direct-wait, failure and OFF tests; `python3 -B tools/test_derby_fixed_integration.py`
for normal-path/native-text preservation and image-proof negatives; and
`python3 -B tools/games/run_derby_fixed.py` for the actual generated fixture and
controller on real QuickJS/renderer with ASan/UBSan and existing draw oracles.
The latter performs the three dependent replays in one process. It checks the
known input, procedural-pixel and full-panel hashes and all finish/call/cache/
batch counts. Its clocks include host oracle/debug redraws and do not measure
target speed. There is no host scheduler pace wait, so its pace counters are
explicitly zero. Synthetic wait tests exercise all target wait-accounting paths.

The `DERBY finite fixed workload` CI runs those checks, the actual CMake chunk
selection, and a `--m32` replay with a 163840-byte guest cap after installing
system multilib. i386 uses the device's 8-byte JSValue/4-byte pointers and aligned
doubles, but x87 intermediate precision differs: its C/JS input-bit oracle is
disabled and explicitly reported false. The 64-bit run supplies that oracle;
the 32-bit run checks the known rendered pixels, finish, counters and heap cap.
`DERBY_FIXED_SYSTEM_M32=1` selects installed system multilib; otherwise the
existing user-local sysroot helper is used.

`tools/derby_fixed_base.json` binds OFF comparisons to the earlier source tree;
that Git tree must be available when running the integration test. Mandatory
image proof reconstructs the generated bytes and checks their exact ELF
embedding, the native fixture symbols and manifest, alongside existing fixed
seed, mode and BIN/ELF pairing checks. A target ESP-IDF build/link and DRAM/stack
budget check remain required before hardware execution and performance acceptance.
CI also builds the explicit ON image with ESP-IDF 6.0.1, preserves size/memory
evidence and compiler `.su` stack-use records for this module, and runs the
mandatory image proof. Per-function compiler stack size is not a full nested
JS/native call-chain or runtime high-water proof.

After valid target data exists, native requires lower bridge+native combined
self **and** lower direct whole-work elapsed time; batch requires fewer bridge
calls **and** lower direct whole-work elapsed time. Report all three values and
their ranges. If three dependent repetitions cannot distinguish the change from
variation, or overall work does not improve, stop and do not adopt for performance.
Do not expand SIMD or keep adding repetitions to seek a favorable result.
