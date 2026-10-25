# Reuse stable result HUD text

Product commit0249544efbd4e85e52e1dee5d90ed8a723a37b6c is inherited unchanged
from sol, based on main e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267. The only
product line returns from hud()'s res branch after the last existing text cache
has been populated. No attract/odds/photo/math optimization, PR48 prepared
dispatch, PR47 app control or PR42/44 workspace/transport change is included.

fin, F, pick, pts and replay are stable during the result scene. settle runs
before it; next race, replay and demo transitions call enter(), setting need=1.
The storage callback applies only in pad and reenters pad. build() replaces R
and creates fresh text handles. Only hud writes .s; the fourth assignment is
last. This is a last-label-attempted sentinel, not transaction-commit evidence:
the existing code assigns .s before setText. A failed build keeps need=1 and
rebuilds fresh handles. No new retained cache or lifetime is introduced.

The return leaves confetti painting, fade/demo visibility, parent callback work
and sound active. Formatting/string comparison is avoided; native setText was
already skipped when the text was equal. Removing allocations also removes
their possible OOM failures, so strict failure/timing equivalence is not claimed.
Any future in-place result-data change must invalidate/rebuild the scene.

## Reproduction and output

```
python3 tools/experiments/derby-result-hud/verify.py
python3 tools/experiments/derby-result-hud/bench.py --pairs 40
# GCC multilib required:
python3 tools/experiments/derby-result-hud/verify.py --m32
python3 tools/experiments/derby-result-hud/bench.py --m32 --pairs 40
python3 tools/experiments/derby-result-hud/test_device.py
```

One unchanged O2 C binary runs the actual QuickJS/native DERBY app with only the
lowered JS input different. Verification drains two final-panel pipes and
directly compares every64,800B panel, including equal EOF and successful exits.
All14,564 normal MID presentations match, SHA256 of their concatenation
7f77895204acb48d5447ae901cd8218182c8149647fcc14641d3cbec10ada16d;
procedural CSV, input/demo/replay/next-race/Back events also match. The old
procedural pixel hash alone would not cover the HUD, hence the final-panel test.

One lifecycle case changes points during res and sets need=1: all1,509 composed
panels match (SHA256ebc3b44915fc9144eb8acb43696430152d90f5ae6a900f4a7ce0c762a2781540).
A test-only observer records arguments after successful native text/setText
calls. The same mutation without need=1 is the negative: baseline refreshes,
candidate is correctly rejected for stale native text. This codifies the
invalidation requirement. Observers and panel streaming are absent from timing.
Independent read-only lifecycle/verification review found no blocker.

## Host measurement

The timing binary removes host JS draw/node oracle wrappers and immediately
returns from the native debug hook. The real app frame and native UI/procedural/
DERBY/input bindings execute. It times JS_Call(frame), including the existing
libc/TLSF-charge allocation instrumentation. Input pump, pending jobs/end-turn,
raster/presentation and panel hashing are outside. No timed logs, heap walks or
forced GC; fresh process/context each trial, two warmup pairs discarded,40 warm
frames then300 recorded frames, AB/BA order, fixed simulated33.333ms steps.
This is host frame-call time, not full frame/LCD/FPS or product GUEST_ALLOCATOR.

Native GCC14,40 pairs: result trial-mean medians56.484→51.456us; paired ratio
0.91293, paired bootstrap95% [0.88831,0.92410] (5000 resamples,seed423).
Requests/frame111→78, charged B5956→4228 (**-33/-1728**). Peak rise2444B
unchanged. Paddock control requests126/charged6720/peak2204B are unchanged,
ratio0.99443 [0.98107,1.01278]. Earlier12-pair result ratio0.90407
[0.87745,0.96867] agrees in direction and allocation delta. Frames within a
trial are dependent; descriptive pooled p95 is not an independent-trial CI.
These results apply to the warmed result scene; no race or device FPS benefit
is claimed. CI retains complete per-frame samples and final-panel identities.

## Finite device handoff

Isolated diagnostic copies reuse PR54 e80ad33f09eabee85e905fa243d28986321758fa /
PR50's boot method. Workload `derby-app-result-120-v2` runs through product
UI-owner app_tick, scheduler, GUEST_ALLOCATOR and LCD. Seed0x2545f491, race1,
oval and MID. Actual warm frames load gate/runner plans; after40 and empty queue,
unmeasured model steps advance to finish, using real step/settle/enter(res).
The photo animation is skipped in this setup; it is a selected result snapshot,
not normal-game progression/FPS. Ten actual result frames initialize labels and
confetti, then120 actual frame calls are measured. The model advance is capped
at6000 steps and readiness requires res/manual mode, no load queue, no need and
the existing label cache. External key snapshot is empty but native input APIs
remain; storage reads no saved state, writes no-op, audio silent.

Exact diagnostic JS/native-input host test completes120frames on both sides,
with equal procedural/CSV and composed-panel hash33c5d4ed94087e34. Both call
down120/query1440. This different seed/result saves32 requests/1672 charged B
per frame; do not assume the host seed3 result's33/1728 is universal. This test
does not emulate product allocator, scheduler or LCD. Generated files are
byte-identical A/B except the owner's source-head string; the original product
scene file differs by the one guard. Manifest records its SHA256 separately.

The existing bounded broker observation is30seconds/32KiB/256lines. Boot starts
once; six summary lines plus at most16 guest console calls. START must identify
full source/workload. Success requires pointer_bytes=4, frames120, input_down120,
input_queries1440, END status=complete frames=120 complete:true, then EXIT running=0.
The24-second cooperative deadline rejects even a late final frame; it cannot
preempt a blocked turn/output. Error, timeout, wrong counts, pending presentation
or missing markers is incomplete. Do not use derby_fixed parser or relax guards.

frame_wall_us brackets the original frame with native markers, including marker/
call overhead, post-frame lead reduction and scheduler gaps. median_x2/2 is the
exact median us; p95 is sorted sample114 at120. Window includes pacing, ending
before last presentation; owner_tick_us includes normal app_tick/LCD work.
Allocator counters delegate unchanged callbacks; bytes are requests, not TLSF
occupancy. Boundary free minimum/free_end are not transient peak heap. Sorting,
logs and heap queries are outside frame timing; instrumentation is common A/B.

CI produces separate product/result A/B ZIPs with ELF/map, generated patch/
manifest, source/per-file hashes, sdkconfig/cache and flash inputs/checksums.
IDF6.0.1, INFO startup, product partition/security/heap/stack settings and all
cache/affine switches remain unchanged. Bootloader0x0, table0x8000, app0x10000;
3MiB factory/dictionary/font/storage layout unchanged. Product images boot HOME
and local.derby/menu12 without auto measurement, retaining normal storage.
PR47 app control is absent. Parent owns hardware capture/stop/recovery; this
experiment performs no USB, flash or merge.

## Diagnostic setup revision v2

The v1 firmware performed hundreds/thousands of model steps synchronously; a
related real-device baseline reported InternalError interrupted during setup.
This is a shared risk, not proof of the precise watchdog cause. All previous v1
diagnostic ZIPs are superseded. Product optimization and host benchmark remain
unchanged. Setup now yields to the existing UI owner after at most8 actual model
steps per original frame callback, with total6000-step bound. Result/photo also
advance the remaining settlement field in those batches, then call original
settle() only after done8 or the existing time200 cap. Update/interpolation order
is preserved. No watchdog/guard/heap/stack/scheduler budget is disabled or raised.

A host-only observer counts actual step() calls, requires max8 and multiple
turns, and removes itself before measured frames. The owner deadline remains24s
including setup; a late or unfinished run is incomplete. Batching can increase
wall time, so final target completion/watchdog fit still requires capture. The shared photo regression additionally checks an unbounded-setup negative.
Use only the new exact-head v2 artifacts; ordinary product behavior is unchanged.


## Current-main comparison (2026-10-25)

The active comparison baseline is `6db9244d585fa3caad899853ff8c7fbe96c5d7d3` (merged PR65/68/69),
not the historical e0fe98a8 baseline reported above. The candidate keeps that
main product and adds only this PR's original application change. Prior numerical
results remain historical; they do not establish the current-main device effect.
The existing dedicated CI now checks both arms against this pinned baseline.

Reuse the finite workload `derby-app-result-120-v2` unchanged: 120 measured frames,
production guest allocator, scheduler/owner and LCD path, 24-second cooperative
deadline, normal product INFO/partition/security settings and existing guards.
No synchronous model preparation, budget bypass, persistent storage writes or
new normal-firmware measurement hook is introduced.

Device comparison: verify source/ELF/manifest and complete retained logs for each
boot; use one A1/B1/B2/A2 block with fresh boot contexts and identical input.
Compare summed `owner_tick_us` over the complete matched 120-frame windows.
Keep each boot's JS median and p95 separate. `window_us` includes pacing and is
not an FPS measurement; boundary-free samples are not exact transient peak heap.
Accept a runtime improvement only when both paired directions agree and the
owner reduction exceeds observed same-arm variability, with matching semantic
markers and no new error, continuation, timeout or unexplained memory regression.
Otherwise hold the candidate and inspect the discrepancy; do not reinterpret
JS-only gains as whole-device benefits or add retries merely to obtain a win.
Existing host panel/state/boundary oracles remain the output-equivalence evidence.
No device result or merge is claimed by this preparation.
