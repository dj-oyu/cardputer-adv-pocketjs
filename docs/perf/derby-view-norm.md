# DERBY view norm reuse

Hypothesis: reuse `sqrt(f*f+25600)` across the native `ou()` culls belonging to
one camera view, preserving the original double expression and draw stream.
The course cache and project batch experiments remain OFF in normal builds.

The comparison is pinned to main `e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267`.
PR49 `ebd42393eb9b382e39737b45e3e092e86c9908c3` is a different branch
(80 commits ahead, 2 behind; common ancestor `5455a599`). Its prepared startup
benchmark and allocation figures are not this application's frame baseline.

`ou()` caches the double norm lazily. `view()` invalidates it before reading
camera getters, and each cull checks the current `f`. This latter check matters:
a camera-index getter can reenter series culling after outer `view()` has already
written a new focal length but before it rebuilds the chord coordinates. Merely
hoisting the square root after validation would give this call a stale norm.
NaN misses the key comparison and retains the original repeated evaluation;
signed zero gives the same positive norm. A straight course never calls `ou()`.
No heap storage or public API is added; the view's two doubles add 16 static bytes.

Reproduce, without a device:

```
python3 -B tools/games/run_derby_view_norm.py --sanitize --pairs 1 --out .cache/norm-asan
python3 -B tools/games/run_derby_view_norm.py --pairs 9 --out .cache/norm-timing
```

The runner builds both revisions with the same source flags and real QuickJS,
DERBY app chunks, input pipeline and Kasane renderer. It forces oval/straight,
HEAVY tier and panning WIDE cameras. Pair zero captures all double draw inputs
and per-frame CSV; these and pixels, race results, allocations and bytes must
match. Later pairs alternate order without capture/CSV and retain every frame's
latency and guest allocation counts. The host allocator is the existing TLSF
length **model**, not the device's GUEST_ALLOCATOR. Draw validation hooks run
multiple render paths, so frame timings are diagnostic host timings, not device
scheduler/LCD timings. Native view/ser timers include any draw-hook work.

The sanitizer pass also compares the JS ser oracle on every series call and
4,000 random camera/course cases per captured run. The focused C test covers
1,848 clip-boundary cases including nextafter neighbors, infinities, NaN,
signed zero, subnormal and overflowing focal lengths, plus view replacement,
invalid view, straight/oval transitions, reset and reentrant getters.

Confirmed app reach at this workload: oval has 383 views and 20,159 culls over
1,303 race frames; norm evaluations fall from 20,159 to 383 (98.10%). Straight
has 339 views, no culls and no norm evaluations over 1,305 race frames. Source
hashes, compiler flags, affinity, raw timings, allocation counts and draw hashes
are retained in the runner output. Host frame speed is not an acceptance claim;
acceptance remains pending repeated target measurements.

The `DERBY view norm reuse` workflow packages exact-head baseline and candidate
**product** firmware, including bootloader, product partition table, flash
arguments, sdkconfig, ELF/map and SHA256 checksums. Product security defaults and
INFO startup are inherited; no USB operation or broker modification is included.
The parent queues these artifacts through the existing MCP with each observation
at most 30 seconds / 32 KiB / 256 lines. Compare the same app, course, camera,
seed and load tier; report target frame latency separately from host counts and
from PR49 startup. No target result has been measured here.
