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

## Device artifact boundary

CI packages ordinary baseline/candidate product images with identical defaults,
IDF6.0.1, INFO startup logging, real guest allocator and unchanged partitions,
security and budgets. These boot HOME; local.derby is menu index12. PR47 app
control is absent. ZIPs include app, bootloader, table, flash arguments, ELF/map,
config/cache and checksums/provenance. Verify source heads and app/ELF hashes;
standard offsets0x0/0x8000/0x10000 are also recorded in flasher_args.json. No
flash or USB operation is performed here.

These are ordinary images for functional/preflight work, **not a complete fixed
performance experiment**. Existing integration finite-fixture generation at
tools/derby_fixed_workload_chunks.py replaces attract(b) with fixed state, so
it cannot measure this path. That discovery stops reuse of that fixture. Shared
CI/generation/frozen references remain untouched. A device comparison needs an
owner-coordinated diagnostic retaining attract and fixing input/seed, or a
separately accepted normal-app protocol. Do not compare random races as equal
work or infer device benefit from these host samples. Normal app storage
behavior remains (reads and saves); no storage operation is added by the patch.
