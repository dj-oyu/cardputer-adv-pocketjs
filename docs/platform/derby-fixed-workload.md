# Finite fixed DERBY workload (diagnostic only)

The generic `POCKET_DERBY_FIXED_WORKLOAD` option in `main/CMakeLists.txt` defaults
to OFF. These three diagnostic-only source branches explicitly select ON in
their root CMake, using the same non-FORCE cache declaration as their existing
telemetry/measurement/fixed-seed modes. Registered fixed-argv builds therefore
select the finite fixture without caller flags or project-setting changes.
The mandatory `diagnostic_mode_proof ALL` requires workload ON and its exact
fixture bytes, manifest, native symbols and paired BIN/ELF. Explicit/cached OFF,
a missing mode setting or an ordinary image cannot pass this diagnostic source
build, including an incremental build. A failed proof prevents the registered
builder from publishing a successful artifact manifest, even if image files
were produced. Normal branches and the generic option remain OFF.

The fixture requires measurement, fixed hardware input and ordinary app chunks.
Normal OFF keeps the original owner/app path and native
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
root evaluations at the clip/cache normalization hooks, cache guard hits,
cache guard misses, view cache refreshes,
runner batch calls and used batch rows. They count only inside the measured
interval. A view refresh is not a cache guard miss. Baseline and batch have no
clip cache; their c4 count is one normalization square root per clip predicate.
This c4 value is not a count of all square root evaluations in the app or
renderer. The original
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

The public parser is `tools/games/derby_fixed_result.py`. Its parsed result
schema is `tools/games/derby_fixed_v1.schema.json`; the successful collector
receipt schema is `tools/games/derby_fixed_transport.schema.json`. Complete host
examples are `tools/games/fixtures/derby_fixed_v1.log`, `.parsed.json` and
`.transport.json`. Their receipt is an illustrative assertion, not evidence
of a hardware acquisition. `tools/test_derby_fixed_protocol.py` validates both
schemas, the sample and rejection of transport loss/reset/truncation/limits.

Protocol v1 is ASCII, space-separated `key=value` fields, one LF-terminated
record per line, no duplicate/unknown/missing fields, at most 1024 bytes per
diagnostic line including LF. Successful order is exactly three `DERBY_FIXED`
records with `rep=seq=1,2,3`, then one `DERBY_FIXED_END`. Summary field names and
string types are listed in the parsed schema's `$defs.summary`. All summaries
must use one variant/session/fixture/manifest; identity and zero quality fields
are fixed by the schema. The END fields are exactly `v=1 session=<same serial>
reps=3 valid=1 quality=0 independent=0`. An abort may emit an invalid END without
all summaries; that is a rejected whole trial. uint64 range checks, matching
session/variant, END/order and `work_us + sum(pace_us) <= wall_us` are semantic
parser checks in addition to the structural JSON schema.

`parse_trial` and `parse_collected` share the comparison work gate with the
host runner. Each repetition must have the following c0..c9 counts:

| Variant | c0..c9 |
|---|---|
| baseline | 0,120,600,7735,7735,0,0,0,0,0 |
| native | 0,120,600,7735,120,7735,0,120,0,0 |
| batch | 0,120,600,7735,7735,0,0,0,120,960 |

Bridge calls are 2086 for baseline/native and 1246 for batch. Bridge, native
and procedural-build calls are fixed work and must match across the three
repetitions; native and build work cannot be absent. Guest calls must equal
120 plus the recorded continuation dispatches. VM calls must cover the build
calls, raster/transfer calls must cover the 120 completed contexts, and measured
output calls must be zero. VM/points/raster/transfer/observer calls can depend
on continuation slicing or host/device instrumentation; their absolute values
are not frozen to the host profile or required to match between repetitions.
The no-continuation host runner additionally checks model-local determinism.

Each stage self time must fit within wall time, zero calls imply zero self
time, and the sum of the mutually exclusive stage self times must fit within
direct work. No inclusive time is added. These checks reject impossible stage
values; they do not calibrate or subtract clock overhead.

Only comparison-qualified results have `format_valid=true`,
`work_verified=true` and `all_repetitions_valid=true`; the public result schema
requires these flags. `parse_trial_format` exists for synthetic controller
tests and returns `work_verified=false` and `all_repetitions_valid=false` even
when its format/identity/time checks pass. Such output cannot qualify a
performance comparison. The collector and CLI always use the strict work gate.

The owning collector starts bounded acquisition before app start through the
existing formal operation. Protocol v1 limits acquisition to 200 seconds and
65536 raw bytes; the device independently caps its fixture at 180 seconds.
It must stop on the first END and preserve the full received bytes, including
unrelated console lines. There are no automatic retries. A final LF/END alone
does not certify that acquisition was complete. The owner must independently
establish `complete=true` and `gaps=overflow=truncated=partial_tail=disconnect=
reset=refused=false`, plus raw byte count, SHA256 and elapsed `duration_ms`.
Bind that acquisition to the expected source head, paired image proof and
variant outside the text protocol; never mix boots or infer provenance from
the small per-boot session serial. Device identifiers and broker credentials
belong to the private execution evidence, not a public result or PR.

Call `parse_collected(raw, receipt)` or run
`python3 tools/games/derby_fixed_result.py tools/games/fixtures/derby_fixed_v1.log
--transport tools/games/fixtures/derby_fixed_v1.transport.json` to validate the
receipt's quality, bounds, byte count and digest before parsing the whole trial.
The legacy `parse_trial(..., transport_complete=True)` interface is for an
owning caller that already established transport completeness. Neither parser
can establish those physical transport facts from a caller's JSON assertion.
No capture, injected input, storage, raw USB bypass, flash/restart or permission
change is added by this protocol.

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
doubles. This harness selects SSE2 binary64 evaluation rather than the i386
default x87 extended intermediates. Its C/JS input-bit oracle remains disabled
and explicitly reported false; the 64-bit run supplies that oracle. The 32-bit
run must still match the same known rendered pixels, finish and work counters
under the heap cap. It reports the exact compiler flags and guest cap. No pixel/work expectation is relaxed for this model. This SSE2 binary64,
8-byte JSValue/4-byte pointer/163840-byte guest-cap model is not proof of
Xtensa numeric behavior, device heap high-water or device performance.
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
