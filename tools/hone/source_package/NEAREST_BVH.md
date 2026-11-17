# Nearest-surface BVH candidate

Independent opt-in acceleration around an existing closest-point callback. It
reorders face IDs in exclusive scratch and builds balanced spatial partitions
with at most eight triangles per leaf. Each query visits the closer child first;
equal distances choose the smaller original face ID regardless of traversal.
The callback and its returned coordinates are unchanged. Rebuild every target
frame: no cache survives geometry mutation.

The candidate retains the prior batch accelerator's conservative acceptance
bound, `128 * FLT_EPSILON * scale²`, rather than introducing a smaller pruning
tolerance. It is for kernels obeying that bound; it does not establish a universal
error bound for arbitrary callbacks. Query points exactly representable as
binary32 use downward-guarded float AABB distances and upward-guarded thresholds.
Each positive subtraction/product/sum has an outward one-ULP guard. Non-float
queries retain the double bound; scale above 1e10 uses exhaustive traversal.
Nearest-point calculations, offsets and output precision are never replaced by
these traversal bounds. Compile without fast math or contraction and use ordinary
round-to-nearest IEEE behavior.

Workspace is caller-owned, complete, suitably aligned and disjoint from all
source/output spans; this is a trusted native candidate, not an untrusted parser.
Capacity is computed by `hone_nearest_bvh_bytes`. Build failure leaves workspace
partial; never query it. Query failure leaves the supplied hit/face unchanged.
Diagnostics counters may advance on failure. A 32-entry traversal stack covers
the admitted maximum 65535 triangles. No dynamic allocation occurs inside it.

Synthetic `test_nearest_bvh.c` compares 3000 queries with exhaustive evaluation,
including duplicate faces/ties, mutated same-pointer targets, non-float query
fallback, invalid inputs and short workspace. Host O2 passed. Compile with
`-std=c11 -O2 -Wall -Wextra -Werror -Wno-misleading-indentation
-ffp-contract=off -fno-fast-math`, linking libm; add ASan/UBSan where supported.

An independent private runtime test passed all 63 sampled poses with identical
source positions, projected packet bytes and RGB565 to the earlier full-search
reference. Across 128 calls (including reopen), triangle evaluations fell from
412448 to 82831. Host median nearest-batch time was 86.90 us for the previous
linear-box implementation, 71.88 us for double-bound BVH, and 59.84 us for guarded
float-bound BVH. These non-exclusive host observations are not target FPS claims.
The private model's workspace grew by 5020 bytes. Actual target geometry phase
timing and memory must be measured before adoption.

Overlap is a trusted-call precondition, not checked or tested as a rejection
contract. Alignment and short workspace are checked. Callers must enforce
ownership before calling; deliberately overlapping the spans is unsupported.

`test_nearest_bvh_edges.c` retains the original fixture unchanged and adds a
finite extreme-scale exhaustive query, a no-finite-hit oracle failure with
unchanged outputs, misaligned workspace rejection, and 20000 comparisons of
float box bounds with the original double bounds. These are focused regressions,
not a proof for all floating-point inputs.
