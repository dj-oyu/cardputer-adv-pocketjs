# Q7 benchmark-only preflight

This is not production enablement, hardware qualification or a 20 fps claim.
The published `experimental/mesh-q7-certified-default-off` branch stays unchanged.
Broker test branches must differ only in the prepared/Q7 option defaults:

- original: PREFLIGHT ON, PREPARED OFF, Q7 OFF, EDGE4 OFF;
- Q7: PREFLIGHT ON, PREPARED ON, Q7 ON, EDGE4 OFF.

Both are **instrumented strict-FP** builds. Enabling preflight also applies the
same strict compiler flags to original depth, retained setup and Q7 sources in
both arms, so the original arm is not byte-identical to ordinary production.
Require fresh full builds, verify actual cache values/compiler commands and
symbols, record commit/tree/ELF/bin hashes. Do not infer flags from defaults.
No PIE backend is involved.

## Stop conditions before measurement

The optional debug `meshQ7Preflight()` executes a public synthetic numeric test across ordinary frame turns
before MB READY. It compares production Q7 with copied certificates disabled:
RGB565, F32 depth bits, owners, return values and safe fallback. This checks only
the certificate optimization; it does not compare Q7 coverage against the
original float renderer. Full floating environment is restored. S3 FCR and FSR are read,
not changed by the wrapper; both before/after values must match. No undocumented DAZ bit is toggled.
The effective double/F32 subnormal probes and production environment gates are
the DAZ/FTZ contract. Numeric equality, restoration and actual certificate fast
paths must pass or the app stops before timing. Host non-nearest and SSE DAZ/FTZ
exercise safe fallback, not a claim about S3 registers.

Workspace size/alignment are queried. The incremental host implementation reports 7120 bytes, alignment 8;
query and verify the target ABI independently. Allocate workspace + alignment - 1 with overflow checks,
explicitly align an interior pointer, and retain the raw allocation for free.
Do not rely on heap_caps_malloc alignment (SDK only guarantees 4 bytes here).
Admission requires internal 8-bit heap free >= padded allocation + 20424 +
32768, largest block >= padded allocation, and current task stack historical high-water
>=8192 bytes. The 20424 subtotal is this scene's two 8-face Q7 snapshots, two
4096-byte packet buffers, one-row depth and eight-row owner workspace and temporary retained
setup. It excludes legacy frame storage, VM/model, display, allocator overhead,
other tasks and concurrent allocations. The 32768 margin is a conservative stop
condition, not a mathematical proof of memory fitness. Allocation failure stops
preflight. The numeric allocation is freed before rendering.

Heap free/minimum/largest, stack watermark, payload accounting and cumulative
Q7 counters are sampled at READY, every 30 measured frames and END. Samples
occur outside app evaluate/submit cycle intervals, but logging and sampling can
still affect wall time and scheduling. Both arms must use the same diagnostics.
Capture serial failures/watchdog/reset/OOM and stop; do not relax admission
without reviewing the actual target evidence. The numeric step still performs exactly one paired one-row comparison, at most
one two-face setup and two 240-pixel one-row renders. All 3376 steps and all
810240 compared slots remain mandatory. The native wrapper groups at most 32
steps per ordinary guest frame. Its soft wall-time budget is 4 ms; another step
is admitted only if twice the estimated step cost plus 100 us fits. The estimate
is the larger of the latest step and 75% (rounded upward) of its previous value,
so one scheduler outlier does not permanently force one-step batches. The actual
maximum remains separately reported. This estimate is not a future-cost bound. At least one step is attempted unless
the run has already expired or failed. A 32-step batch spans at most two scene
setups and 30720 triangle-pixel visits, plus comparisons and fenv handling.

Begin and every step save/restore the current guest caller fenv; a separate
numeric fenv persists in the owned workspace between turns. Raw FCR/FSR equality
is checked cumulatively around begin and every step. The frame callback returns
after one bounded batch and does not evaluate/submit benchmark geometry until
completion. No numeric work runs inside guest startup. No sleeps, watchdog
changes, manual feeding, or repeated debug calls in one guest turn are used.

A native batch over 12 ms, an individual step over 20 ms, or total preflight
over 45 s causes failure and frees the workspace. The batch limit is stricter
than the individual-step limit. These are retrospective stop conditions, not
hard execution-time bounds. Total expiry is also checked before doing another
numeric step. The batch timer includes initial admission/allocation/begin but
excludes final JS result construction. Existing 2 s startup and 250 ms turn
watchdogs are unchanged. Session teardown frees abandoned state before the VM
is destroyed. A JS object-allocation failure does no numeric work and leaves
any existing state available for teardown/retry.

Reports include batch count, maximum batch steps, maximum step/batch time and
total elapsed time. With 32 steps per frame, the work-count minimum is 106 frame
calls; slow target steps can cause smaller batches. No target-duration prediction
is implied. The former one-step-per-frame plan needed about 111 seconds and did
not fit the broker's 90 s lease / 87 s run limit. Neither limit is increased.
The 45 s preflight stop leaves nominal room for warmup, measurement, logs and
capture; the device owner must account for actual boot/session overhead and
completed-frame time. If the corpus cannot finish within budget, report failure
rather than skipping cases, widening limits or reporting benchmark results.

Production prepared/Q7 snapshot allocations also require alignment 8. Their
feature-gated allocator uses heap_caps_aligned_alloc with MALLOC_CAP_DEFAULT on
ESP, then zeroes exactly the payload, with compatible free. Host calloc relies
on the standard max_align_t guarantee. OOM injection order, copied ownership,
candidate/committed swap and rollback remain unchanged. Payload sizes do not
grow, but target allocator overhead/fragmentation are not inferred from that.
Ordinary default-OFF allocation behavior is unchanged.


## Same scene and visual reference

The public meshbench model remains 1280 bytes, SHA256
`43495668e4acc308672f9b3ae00784794c47b2b42c30f9c01fc0a2ab1bce4536`.
It has four quads/eight triangles, fixed color/depth and the existing 150-frame
camera sequence; 30 warm-up plus 120 measured frames. Model bytes and frame
inputs must match in both builds. This low-complexity synthetic scene does not
prove fox-model performance or broad Q7 quality.

Run `python tools/kasane_contract/run_meshbench_q7_compare.py`. It checks each
of the 150 panels against the original renderer in eight display/IO modes,
for each renderer, under ASan/UBSan (LSan disabled; allocation ledger checked).
It writes final zero-based frame149 as 240x135 little-endian RGB565, top-left
row-major, in each `.cache/meshbench-reference-*` directory. Compare exact panel
capture when available. A camera photo can show gross corruption but cannot
establish pixel or depth equality. After MB END the app holds that last frame;
request capture before stopping. No private models are in these tests.

Retain the broader Q7 approximation caveats from
`mesh-q7-certified-experiment.md`: coverage and RGB are deliberately quantized;
a successful certificate preflight is not original-float visual equivalence.

Telemetry is explicitly cumulative. `warmup_ack` runs in the next callback
before frame31 submission; `submitted` snapshots run after commit and can precede
rasterization of that submitted frame. `end` runs in the subsequent callback
with the final panel held. Frame counters identify submissions, not display
completion timestamps. Heap minimum and stack high-water are lifetime values,
not isolated per-window peaks. Do not treat evaluate/submit cycle sums as total
frame time or infer fps from them; use existing display completion/wall-clock
measurement from the device owner and state its exact interval.

The ESP aligned snapshot allocation bypasses optional shiki_memstat calloc
attribution. Explicit mesh payload counters and heap-capability telemetry still
apply; do not treat that optional allocator attribution as total native usage.
The host wrapper test extracts the exact debug wrapper and stubs ESP/JS boundary
APIs to test 4-aligned raw allocations, completion, teardown, OOM and time-stop
cleanup. It does not execute app_session, target registers or FreeRTOS.
