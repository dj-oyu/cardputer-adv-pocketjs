# Nearest-surface host candidate

`nearest_surface_candidate.c` independently computes the closest point by a
projected face interior plus clamped edge candidates. Degenerate triangles
retain their closest edge/vertex. Strictly nearer candidates replace the winner;
exact ties retain the first triangle. No GPL implementation was copied.

The offset subset is ON_SURFACE toward the original query side of the nearest
point, not along an assumed face or smooth normal. Coincident queries remain
coincident. Other wrap modes, scaled object spaces, arbitrary modifier stacks,
vertex-group masks and higher-order subdivision are not admitted.

Compile `test_nearest_surface_candidate.c` with `cc -O2 -std=c11 -lm` (or
ASan/UBSan). Tests cover face/edge/degenerate queries, signed-side offset,
coincident position, invalid late indices and retry. Caller supplies valid
nonoverlapping spans and retains exclusive ownership for the call. No allocation
or retained source pointer. A mesh caller must stage its entire candidate before
publication; single-point output success is not mesh-level atomicity.

This is brute force O(queries*triangles), no acceleration structure. Validate
cost on target hardware before integrating. A topology BVH with refitted bounds
could preserve exact nearest results, but needs validated conservative bounds,
stack/work limits, tie behavior and measured build/refit/query costs. Do not
replace this with a previous-face shortcut without a nearest-distance bound.

Host timings in the synthetic single-triangle fixture are not MCU performance
or a whole-character timing result. Source deformation, normal rebuilding,
alpha/PBR shading, near clipping and LCD transfer remain independent work.

## Eight-face batch candidate

`nearest_batch_candidate.c` provides a caller-owned 24-byte AABB per eight
consecutive triangles. Every call validates topology and refits bounds, even
when pointers are unchanged. There is deliberately no persistent cache or
caller-supplied generation token that could accidentally admit stale bounds.
All inputs and output/work spans must be disjoint and immutable during the
call. Outputs are candidate-local; a late invalid query can leave earlier
candidate points written. Publish only after whole-call success.

Float bounds expand outward with nextafterf. A conservative double rounding
allowance prevents pruning near the distance boundary; ties retain source
triangle order. Extreme coordinates and insufficient workspace fall back to
brute force. This is a bounded-domain host prototype, not a universal numerical
proof or production runtime admission. Tests compare exact coordinates AND face
identity, small/large magnitudes, degeneracy, same-pointer deformation, late
invalid input and retry. Run `python test_nearest_batch.py` and compile/run
`test_nearest_batch.c` at O2 and with ASan/UBSan (leak detection separately).
Host benchmarks must include refit and validation and compare the same batch
API with zero workspace. No hardware speed or full-renderer claim follows.

`test_nearest_provider.py --root <checkout> --out <build-directory>` injects a
synthetic plane-wrap producer into the existing provider caller fixture without
changing its production sources. It checks wrap output against the independent
original geometry, then executes real model evaluation, pending snapshot append,
publication and row rendering including failure/rollback/retry and pin ownership.
`--sanitize` enables ASan/UBSan, excluding leak checks. This is a host integration
seam test, NOT the restored full character, firmware integration or target timing.
Full-character cold chunk identity, deformed corner normals, visibility/alpha and
near clipping still need the scene adapter before runtime admission can open.
