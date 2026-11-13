# Q7 adoption review layer

## Scope and source identity

This layer is stacked on the native mesh foundation (`724d97ad`, plus its CI).
The original Q7 train is `724d97ad..bbee585a`: 12 commits and 40 changed files.
It contains Q7 certification, the bounded diagnostic preflight and environment
handling, row reuse/statistics, and the small IRAM flat-span loop. The rejected
depth-recurrence experiment and subsequent exact-screen-x candidate are excluded.

The measured flat-span source is preserved byte-for-byte:

- Commit: `bbee585a62302b23750642007eef206352a39ef8`
- File: `main/ui/kasane/ksn_mesh_q7_certified.c`
- SHA256: `7be9c6dc950f1c88710f257bf64f80cf985671272b6595ddb4ae22a29fa118b4`

The device trial reported 15.890 fps for that benchmark configuration. This is
not a measurement of the new default-off firmware, an arbitrary model, or this
CI head. Preserve the measured commit and device evidence rather than attaching
its performance result to an unmeasured build.

## Explicit opt-in, safe defaults

The benchmark branch enabled PREPARED, Q7_CERTIFIED and Q7_BENCH_PREFLIGHT by
default. This review restores all three to OFF; EDGE4 remains OFF. No renderer
formula, certificate, FP policy, workspace rule, ABI, security setting, or
partition is changed by that configuration cleanup.

To build the benchmark profile explicitly:

    idf.py -B build_q7 -D SDKCONFIG=build_q7/sdkconfig \
      -D KASANE_MESH_PREPARED_PROTOTYPE=ON \
      -D KASANE_MESH_Q7_CERTIFIED_PROTOTYPE=ON \
      -D KASANE_MESH_Q7_BENCH_PREFLIGHT=ON \
      -D KASANE_MESH_EDGE4_PROTOTYPE=OFF build

Use a new build directory so a cached CMake ON option cannot disguise a default
OFF test. Q7 coverage/color remain approximate relative to the original raster;
certified depth/mask and optimization-to-predecessor equality are different
claims. Passing the synthetic scene is not a universal visual-error bound.

## CI and remaining adoption gates

`mesh-q7.yml` runs plain and ASan/UBSan numeric/certificate, bounded preflight,
workspace, flat-span/failure/resume tests; separate jobs cover the independent
FP-register model, diagnostic scheduling, managed lifecycle, and public scene
comparison. Every job checks out the exact source head and preserves logs. An
explicit-ON IDF build retains compile flags, map, symbols, stack-use files and
source hashes. The inherited foundation workflow builds the default-OFF profile.

Host results are not S3 throughput, cache-disabled safety, stack/DIRAM admission,
or arbitrary private-model acceptance. Review the target compile flags, helper
placement/call chain and memory records before adoption. No main merge or device
operation is performed by these workflows. Same-head CI and independent review
must complete before treating this review layer as ready.

## Post-131 single-buffer port (host validation)

The integration base is main `1ff2fe87ab9c8bbdc27874b3bcbb269775072901`
(including #131). Q7 source input is `828baa3af69c55e7884c53557ea39f91190010a0`.
The front frame is now overwritten at commit, after the presentation join and
pending/repair checks. Q7 preparation remains spare-slot-owned until that point;
`merge(surface)` swaps its snapshot together with depth/prepared state. Rendering
always reads `q7_committed`. Presentation ACK updates bookkeeping, not snapshots.
An image IO failure retains the submitted front; backdrop IO repairs the new
front when commit has overwritten the previous one. Neither path restores an old
Q7 snapshot independently of the actual frame.

The managed Q7 lifecycle test follows those semantics and checks derived OOM
fallback, stale-cache invalidation, commit/ACK identity, image retries, cap
fallback, cleanup and subsequent reuse. It also checks that invalid admission
and an uncommitted next build leave the front unchanged. The inherited original
pixel suite is not a universal exact Q7 oracle: Q7 coverage/color approximation
is unchanged. Run the original suite with Q7 OFF and the dedicated Q7 lifecycle
and public meshbench comparison with Q7 ON.

Defaults remain OFF, and this port does not modify the certified raster formula.
Host validation does not establish target throughput, dynamic RAM admission,
whole-task stack, cache-disabled safety, or arbitrary-model visual acceptance.
A new target build and exact-head CI are required for this integration; prior
828baa3a target-build evidence must not be represented as a build of this port.

An exploratory addition calling the entire `q7_inherited_depth_suite()` failed
at `test_mesh_depth_qjs.c:138`: varying A/B vertex colors with a mask compared
all pixels against the original painter. That call was not present in the old
Q7 lifecycle test and was removed, not relaxed to a larger tolerance. The failure
is retained in the host review log. Q7's existing numeric certificate suite
separately reports original-vs-Q7 coverage/color differences; certified depth
and optimization-to-predecessor equality do not promise painter color equality.
The certified renderer SHA remains `7be9c6dc950f1c88710f257bf64f80cf985671272b6595ddb4ae22a29fa118b4`.
Dedicated lifecycle pixels and the 150-frame public scene oracle remain required;
this does not certify that varying-color scene for Q7 adoption.
