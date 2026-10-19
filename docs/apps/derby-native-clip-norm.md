# Native clip factor reuse experiment

Baseline: local `970a3b5f3d02aa1b8311b186d99a3842499ec56a`, tree
`11ab870bb53b811da89c771f247c390e23c092b4` (same tree as public PR8
`f7d86c147e6498382be07cf323d2bb4818a95136`). This normal-OFF development
baseline is distinct from installed diagnostic `9db14c88`; no hardware was used.

`ou()` evaluates `m * sqrt(f*f + 25600)` at every tested course chord. `f` is
camera-only; `m` varies with series and remains multiplied in the original
order. Cache the double sqrt once per oval view, without reassociation,
float32 conversion, fast-math, new API or racecore changes. Straight views do
not compute it. The cache adds two doubles to static state (16 bytes). Because
camera array getters and host draw hooks can reenter APIs, `ou()` also checks
that the cached focal length still matches before using it. This adds one
comparison per predicate and conservatively recomputes for NaN.

Operation-count evidence, not a hotspot timing result: the current oval has
19 chord ends, so view alone tests 18 chords (18 sqrt calls becomes 1). A
synthetic frame with all 18 chords surviving each of five series performs
108 predicate calls (108 sqrt calls becomes 1). Actual visibility changes the
series denominator. A compiler may eliminate some original work; this count
is at source-expression level and does not prove a device-cycle saving.

`python3 tools/games/test_derby_clip_norm.py` extracts the actual C predicate,
compares 1,800,000 decisions with its previous double expression (random values,
zero/signed zero, large focal length, infinities and NaN), checks the operation
count fixture and changed-camera reentry. All pass with UBSan. CI runs it.
ASan/UBSan MID game on the cache implementation also passed: 14,975 game ser
calls / 57,002 draws and 32,000 fuzz calls, bit mismatches 0. Existing JS tests
passed 100,094 lane frames, 2,923 PAN frames and finish-photo invariants. A final
oval run is tracked separately in the handoff; no physical frame/LCD/fps claim
is made. The subsequent reentry guard is additionally covered by the extracted
predicate regression; it does not change the ordinary fixed-camera path.

Next: same-head CI and normal formal build, then a bounded device comparison
using the same seed/step/camera/tier. Native exclusive timing must exclude VM
and raster children; completed-context throughput is not LCD fps. If the
comparison overhead outweighs saved sqrt work, reject this optimization.
