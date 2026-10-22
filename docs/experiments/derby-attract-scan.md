# Inactive attract input scan experiment

Product proposal e5298da7b1f2c1e901ffb30ea63b8997786b3d76, inherited unchanged
from sol's shallow investigation. Base is main
e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267, not integration578d552, PR48 or
PR49. Only apps/derby/derby_play.js::attract changes. No PR51 length optimization,
PR42/44 transport/workspace/cache, native math, odds/bar/up, keymap or budgets.

Manual non-pad frames always reset idle and dk without using the scan result.
Skip K.down().join() and the GK iteration there. Active demos still scan in all
scenes; paddock scanning and Back/save stay. Native pressed/down read one
per-turn snapshot without consuming edges (pocket_input.c109–198). Stale kp from
a manual race cannot change paddock idle: a nonempty key resets idle regardless
of hit; empty keys do not form a hit; the first pad scan refreshes kp before any
demo can start. Independent read-only review found no input-state blocker.

Two intentional differences: clock sampling moves before down() even on scanned
paths, so its timestamp can shift by scan duration; skipped allocations remove
their possible OOM failures. This is not strict timing/exception equivalence.

## Reproduction

```
python3 tools/experiments/derby-attract/run.py
python3 tools/experiments/derby-attract/bench.py --pairs 40
# installed gcc multilib required:
python3 tools/experiments/derby-attract/run.py --m32
python3 tools/experiments/derby-attract/bench.py --m32 --pairs 40
```

One common O2 C binary, baseline/candidate lowered app directories. Each trial
uses a fresh runtime/context. The unchanged full MID oracle tests 14,563 frames:
demo/keys, camera changes, replay/next race and Back. Both pixel hash
789dd8aa345ffcf3 and frame CSV SHA256
f80da9e732cdc19f27a2e38d34f97ba07bac9d63d831e240b43bdb3bec5b0695 match
sol's evidence exactly. A separate integration578d552 reproduction also matched
its own baseline/candidate pair (pixel4f063ab172c34803; CSV00c877d4...), with
no proc or native change, but it is not this PR's base.

The timing fixture removes host JS draw/node oracle wrappers. Native input,
procedural plans, native DERBY and UI calls run normally; the C debug hook returns
immediately. Clock interval is actual JS_Call(frame) only, including its native
calls and allocation callbacks. Input pump, pending jobs/end-turn, raster/output
into the host panel and output hashing occur outside. This is not full-frame or
LCD time. No timed logs, heap walks or forced per-frame GC. Forty warm frames,
then300 recorded frames/trial; two warm-up pairs discarded, AB/BA order. Host
caches are warm and contexts fresh. Simulated now advances33.333ms/frame.

The existing host allocator uses libc plus TLSF charge accounting and a header;
it is not the product GUEST_ALLOCATOR. Timing includes that instrumentation.
Samples record wall/thread CPU time, JS requests, charged bytes and peak rise;
clock overhead is not subtracted. Frame samples within a trial are dependent.
All baseline/candidate panel hashes and scene/demo states must match per pair.

## Independent local results

GCC14 native,40 pairs ×300 frames:

| Case | Baseline / candidate trial-mean median us | Paired ratio | Requests/frame delta | Charged B/frame delta |
| --- | ---: | ---: | ---: | ---: |
| Manual race |104.58 /103.20|0.9845|-7|-356|
| Paddock control |51.07 /51.51|1.0068|0|0|
| Active demo control |98.12 /98.60|1.0057|0|0|

Race paired-median bootstrap95% interval [0.9638,1.0074] (5000 resamples,seed423).
Latency improvement remains unconfirmed. An earlier12-pair race ratio was0.9915;
its exact allocation delta agrees. Race max peak rise is2532 B on both sides;
no peak heap reduction is demonstrated. Allocation elimination is confirmed in
this host model, not yet on device. No FPS or general product speedup claim.

Local summaries and compatibility identity are committed alongside this note;
CI artifacts retain complete per-frame samples/CSV/logs. Local full raw path:
.cache/attract-deep/bench-20261022T195802Z/raw.json.

## Finite device diagnostic

The existing integration fixed fixture deletes attract(b), so it cannot measure
this path. Instead this PR reuses PR50's real-app boot diagnostic method and
isolated templates from commit 1e68b268a4b91ec54e88f408118de3e39b8b6bc5
([method](https://github.com/dj-oyu/cardputer-adv-pocketjs/blob/1e68b268a4b91ec54e88f408118de3e39b8b6bc5/docs/perf/derby-app-diagnostic.md)).
No PR50 math optimization or PR53 odds change is imported. Normal product code,
shared generator/CI/frozen references, heap/stack budgets and renderer lifetime
remain unchanged. The new generator edits only a separate clean checkout.

```
git worktree add --detach /tmp/attract-diag <exact-source-head>
python3 tools/experiments/derby-attract/prepare_device.py --checkout /tmp/attract-diag
python3 tools/experiments/derby-attract/test_device.py
```

Workload `derby-app-attract-race-120-v1` boots once through begin_run(local.derby),
UI-owner app_tick, the product scheduler/GUEST_ALLOCATOR and LCD. Same seed
0x2545f491, race1, forced oval, HEAVY tier, camera8 on both variants. Real setup
frames register plans; after40 warm frames and empty load queue the model is
advanced unmeasured to lead390m. Ten further actual frames settle the scene,
then120 manual-race frames run. This is a selected fixed race window, not
normal-game steady FPS. The original attract(b), native down(), query() and
allocation behavior remain. Diagnostic-only keys_pump replaces its physical
snapshot with zero; JS input APIs are not replaced. Every measured call requires
dm=0 and scene=race. Storage reads return no saved state, writes are no-ops and
audio is silent. Hardware inputs do not select the measured scene.

Six short diagnostic lines plus at most16 guest console calls fit the existing
30-second /32KiB /256-line bounded broker observation. No broker/USB mechanism
is added. START identifies full source head, workload and allocator. END must
say `status=complete frames=120 complete:true`, followed by `EXIT running=0`.
The24-second deadline is checked cooperatively between owner turns; even a final
turn crossing it reports timeout, never complete. It cannot preempt a blocked
turn. Reporting and teardown are outside the measured interval. Missing END/EXIT,
app_error, timeout, input_mismatch or present_pending is incomplete/failed.
Do not feed this workload to the old derby_fixed parser or relax its guards.

Measured native input counts must be baseline down120/query1680 and candidate
down0/query600; success rejects any other counts. Those counters prove that the
real native API was reached or skipped, rather than replacing it with a fixture
constant. The exact diagnostic JS and generated native input copy pass the host
120-frame test: pixel2ecc984362eaf8ce and CSV SHA256
51521411a7289d188e1afa68545503d608cf457f90a219e9906bfde4cbb499c0 are equal.
Expected counts are supplied as an unmeasured startup JS literal; generated
pocket_derby.c, pocket_input.c and guest.c are byte-identical between variants,
so validation constants do not shift native hot-code layout.
Native64 host delta is840 allocation requests/42,720 charged B per120frames;
this is a compatibility/path test, not device timing or guest-allocator evidence.
The original full MID test and host timing/raw samples are retained.

Summary metrics follow PR50: native _diag markers bracket the original frame,
including marker/call overhead, the post-frame lead reduction and scheduler
continuation/preemption gaps. Window time includes owner pacing and ends before
the last presentation; summed owner tick wall time includes normal app_tick/LCD. They are not exclusive CPU cycles or completed
LCD FPS. `median_x2` is twice the exact median in microseconds, preserving half-us;
p95 is the nearest-rank114th sample at120 complete frames, max is exact. Sorting/reporting and boundary heap
queries are outside the frame timer. A static120-element unsigned array costs480B;
input counters add24B static state, with no per-frame sample allocation. Existing
allocator counters forward unchanged callbacks and count requests/bytes including
failures, not physical TLSF occupancy. Boundary free minimum is sampled before
frames; free_end is before teardown, neither is transient peak heap. Counter and
wrapper overhead applies to both variants and is diagnostic instrumentation.

CI builds ordinary product and finite diagnostic A/B images with unchanged
partitions/security/INFO settings, IDF6.0.1, cache/affine switches OFF. Artifacts
include app/bootloader/table/flash_args, ELF/map, sdkconfig/cache, image checksums,
source head, diagnostic.patch and per-generated-file hashes. Check profile as
well as source SHA before device use. Bootloader0x0, table0x8000, app0x10000;
3MiB factory and dictionary/font/storage layout are unchanged. Normal product
images boot HOME -> local.derby menu12, have no auto benchmark/END marker and
retain ordinary storage behavior. PR47 app-control is absent. Parent owns all
sanctioned hardware observation/stop actions; no USB/flash/merge is performed here.
