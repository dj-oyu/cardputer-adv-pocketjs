# Registered course cache and double projection batch (opt-in)

This comparison follows normal-OFF PR8 (`f7d86c14`, tree `11ab870b`), independently
of the frozen PR13/14/15 diagnostic heads and PR16's integer runner geometry.
The two switches default OFF and can be evaluated separately or together:

| Switch | Boundary |
| --- | --- |
| `POCKET_DERBY_COURSE_CACHE` | Course registration precomputes each fixed chord endpoint's world x/z and tangent at w=0; view performs only camera transformation. |
| `POCKET_DERBY_PROJECT_BATCH` | Two PAN calls project six finish/distance-post points and eight runners, replacing per-point JS pose/pj result arrays. |

Neither switch changes race state/RNG, camera selection, division, sqrt,
LOD, clipping, sorting policy or procedural programs. In particular, the
course cache is separate from PR14's clip-normalization sqrt cache; the
projection batch adds the pose/camera/projection boundary PR15 leaves in JS.
No new camera feature or full integer perspective projection is included.

## Cache lifetime and memory

The cache lives inside the course's existing single allocation: four doubles
(32 bytes) per chord endpoint for curved courses, no new header or separate
allocation. The game's 19 endpoints add 608 native heap bytes; the maximum
32 endpoints add 1,024 bytes. Straight courses add zero. Registration validates
the entire replacement first and computes against that new registration,
then publishes it and frees the previous block. Failed replacement retains
the previous course/cache; successful replacement clears the view; reset
frees both together. During replacement both old and new allocations can
temporarily coexist. No JS values or source array pointers are retained.

## Projection contract

`project(progress[], depths[], subtract, Float64Array output)` reads 1..32
finite pairs and writes four doubles per point: numerator, Z, screen x,
reciprocal Z. It performs `pose(progress[i]-subtract, depths[i])`, the existing
pj expression, then `r=1/Z` and `screenX=numerator*r`. Double precision and
expression order remain unchanged; no fused multiply-add, reciprocal
approximation, rounding or saturation is added. Negative/zero Z and signed
zero follow the existing arithmetic; JS keeps the original strict visibility
tests. The final post's `n[0]/n[1]` division remains a division in JS rather
than being replaced by multiplication, preserving its exact operation order.

All progress/depth values are read before borrowing output bytes. Course and
view generation checks reject getter-driven replacement. Output must be a
live Float64Array with enough bytes; typed-array type, offset, length and
backing buffer are checked after getters, including detach. No JS callback
runs while the output pointer is borrowed. Immutable backing buffers are
rejected before any native write, including offset typed-array views.
Invalid input produces no internal
partial write. The C scratch arrays are bounded at 32 pairs (512 data bytes);
target compiler stack reports are retained by CI.

The JS path retains reusable 32-double runner and 24-double post outputs
(448 data bytes), plus two fixed six-number input arrays and their JS
headers/code. Existing runner progress and depth arrays are consumed directly
by C: there is no per-frame input pack loop. Six/eight points use two calls;
native input reads, typed output validation/writes, and JS output consumption
are included in the host boundary benchmark. The caller continues to sort
the same eight indices and submit the same visible plans in the same order.
The normal game chunks stay byte-for-byte unchanged when batching is OFF;
only the generated opt-in PAN chunk differs.

## Checks and interpretation

Run:

```
python -B tools/games/run_derby_projection.py --all-tiers --out artifacts/projection
python -B tools/games/run_derby_projection.py --benchmark-only --out artifacts/projection-cost
```

The four switch combinations run the actual QuickJS, real adapter and
renderer under ASan/UBSan, including the existing 4,000-case series oracle.
At every tier, raw handle/input/surface/draw-order streams are byte-compared,
per-frame pixel/work CSVs must match, and finish order/times must match.
MID also checks replay and adds a run comparing every native point directly
with the original JS pose/pj expressions (including signed-zero equality).
LIGHT/HEAVY use the existing non-replay harness scenarios.

The preflight checks over 19,200 points on straight/bend/section boundaries,
close/follow/horizontal-orbit trajectories, Z=.02 and either side, zero and
negative depths, and nonfinite/malformed inputs. It verifies detached buffers,
getter-driven course/view replacement, late-invalid inputs, immutable backing
from both QuickJS APIs, typed output subview guards, maximum cache allocation,
failed replacement, straight replacement and reset. Cache lifetime is checked
in all four independent modes, with explicit 0/1024-byte expectations. Camera
fixtures exercise the current horizontal PAN contract, not a new XYZ camera.

Optimized, unsanitized costs are recorded separately and run serially; they
cover the complete JS/native argument and result boundary, not a raw math
kernel. Sanitized full-game heap/churn observations are functional host
model measurements. None establishes S3 CPU-cycle share, device heap
acceptance, frame rate or adoption. The old WIDE2 JS bytecode percentage
cannot be reused as the current projection share. Actual device comparison
and its separate metadata/approval/cache prerequisites remain pending.

CI pins the PR's exact head, runs all four source switches, builds each with
ESP-IDF 6.0.1, and retains BIN/ELF hashes, cache settings, stack output and host
comparison logs. The existing diagnostic comparison heads are unchanged.
