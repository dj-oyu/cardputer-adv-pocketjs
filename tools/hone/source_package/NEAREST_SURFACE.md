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
