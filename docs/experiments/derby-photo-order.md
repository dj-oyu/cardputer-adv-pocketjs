# Reuse settled finishing order in the photo HUD

Exact inherited product commit f0a49f362eef626ec83517112aae5b3b1bc76d11 is
isolated on main e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267. Only photo HUD's
order(rs) is replaced with fin.o. Result HUD/attract/pad/math changes, PR47
app-control, PR48/49 prepared dispatch and PR42/44 workspace/transport changes
are absent. The product optimization is one line; diagnostic copies are separate.

settle() synchronously computes fin.o=order(rs) before the only enter(photo)
call. Photo frames change neither rs.tc/rs.x nor fin.o. The existing comparator
uses crossing time, descending distance for unfinished runners, then index for
ties. Even time200 settlement with unfinished runners therefore has the same
result. Consumers use indexes/indexOf without sorting or mutating fin.o. Replay,
next race and demo exit leave photo and recreate state. Future photo changes
that advance/mutate the race must revisit this invariant. Eliminated allocations
also eliminate their possible OOM failures; strict failure/timing equivalence
is not claimed. No new retained cache or invalidation lifetime is introduced.

## Reproduction and compatibility

```
node tools/experiments/derby-photo-order/boundary.mjs
python3 tools/experiments/derby-photo-order/verify.py
python3 tools/experiments/derby-photo-order/bench.py --pairs 40
python3 tools/experiments/derby-photo-order/verify.py --m32
python3 tools/experiments/derby-photo-order/bench.py --m32 --pairs 40
python3 tools/experiments/derby-photo-order/test_device.py
```

One common O2 native binary runs baseline/candidate lowered JS. Verification
drains both composed-panel pipes and directly compares every64800B panel,
equal EOF and successful exits. All14564 normal MID presentations match,
alongside procedural CSV and demo/input/replay/next-race/Back events. Native
concatenated panel SHA256 is
7f77895204acb48d5447ae901cd8218182c8149647fcc14641d3cbec10ada16d.
The procedural-only hash would not verify HUD output, so final panels are used.

One generalized boundary table executes the actual model/settle/HUD source:
completed ties and time-capped partial finishes, every8pick, t0/8/49/50/110,
each twice. All160 paired label comparisons pass and fin.o remains unchanged.
Independent read-only product/harness/diagnostic review found no blocker.

## Host measurement and limits

Fresh process/context each trial, alternating AB/BA, two discarded warm pairs,
40 real photo warm frames then60 measured frames. Photo exits naturally at111;
the benchmark does not extend/freeze the scene. It covers the warmed interval
(t41..100), including its text switch at50. It times JS_Call(frame), actual
native bindings and the existing host libc/TLSF-charge instrumentation; raster,
presentation/panel hashing, input pump and pending jobs are outside. Host draw/
node oracle wrappers are removed and the native debug hook returns immediately.
No timed logs, heap walks or forced GC. Fixed simulated33.333ms frame steps.
This is host frame-call time, not ESP32/LCD/FPS or product GUEST_ALLOCATOR time.

Local GCC14 O2,40 pairs: trial-mean medians62.197→59.614us, paired ratio0.95928,
paired bootstrap95% [0.92416,0.99189] (5000 resamples,seed423). Requests
116.7→107.7/frame (-9), chargedB6694.2→5706.2 (-988), peakrise2032B unchanged.
Pad control ratio0.99336 [0.94781,1.02907], all allocation counters unchanged.
A second40-pair repeat gives0.92551 [0.88178,0.96676], identical allocation
deltas; pad1.03689 [0.96952,1.07342]. Timing variability remains. Initial12-pair
ratio0.92583 [0.88822,0.98192] agrees in direction. No device/FPS benefit is
claimed. Frames within a trial are dependent; pooled p95 is descriptive.
Historical local raw identities and summaries are retained; CI produces fresh
native/m32 samples using the published scripts and records exact input/binary
hashes. ABI measurements are not cross-device time predictions.

## Finite device diagnostic

Isolated copies reuse PR57/PR54 and PR50's product UI-owner boot method.
Workload derby-app-photo-60-v1 uses product app_tick/GUEST_ALLOCATOR/scheduler/
LCD. Seed0x2545f491, race1, oval, MID; native input APIs execute with the empty
physical snapshot. Storage reads no prior value/writes no-op and sound is muted.
After40 real warm frames and empty load queue, an unmeasured capped6000-step
actual model advance captures the winner's crossing with the existing photo()
interpolation, then actual settle() and enter(photo). Ten real photo warm frames
precede60 measured original frame calls (t11..70); no scene length is changed.
The60-frame choice respects the natural110-frame photo duration, includes zoom
and the label transition, and differs from PR57's120-frame result workload.
This setup skips race animation, so it is a stage snapshot, not normal-game FPS.

Generated native files and workload are byte-identical A/B except owner source
provenance. Manifest separately records the one differing product scene file.
The exact diagnostic host test requires equal final-panel hash/procedural CSV,
60down/650queries,60 completed frames and the expected native allocation delta.
It does not emulate target allocator/scheduler/LCD. Owner boundary tests cover
24s-1/24s/24s+1, unfinished timeout, wrong counts and pending presentation.

Boot auto-starts once. Existing broker capture limits remain30s,64KiB received,
256lines/32KiB retained. Six short summaries and at most16 guest log calls.
Require START with full source/workload, pointer_bytes4, frames60/down60/queries650,
END status=complete frames=60 complete:true, then EXIT running=0. Missing marks,
timeout/error/count mismatch/pending presentation are incomplete. Do not relax
broker guards or use derby_fixed parser. Cooperative24s deadline checks a fresh
clock even after the last frame; it cannot preempt a blocked turn/output.
Target setup watchdog and complete runtime still need device capture.

frame_wall_us includes common native marker/call overhead, post-frame lead
reduction and scheduler gaps. median_x2/2 is exact median; p95 is sorted sample57
of60. Window includes pacing, ends before last presentation; owner_tick_us sums
normal app_tick/LCD work. Allocation bytes are requests, not TLSF occupancy;
boundary_free_min/free_end are boundary samples, not transient peak heap. Sorting,
logs and heap queries are outside frame timing. The60-sample array is240B and
all instrumentation is common A/B, only in diagnostic copies.

CI builds product/photo baseline/candidate ZIPs with ELF/map, per-file/source/
SDK/flash hashes and patch/manifest. IDF6.0.1, INFO app_init Application information,
product optimization/partition/security/heap/stack settings; all experimental
cache/affine switchesOFF. Offsets bootloader0,table0x8000,app0x10000; factory3MiB,
dictionary0x310000,font0x510000,storage0x590000. Ordinary product images boot HOME,
local.derby/menu12 with ordinary storage; no PR47 app-control. Parent owns
hardware capture/recovery. No USB, flash, merge or shared frozen-reference edits.
