# Immutable eight-row graph index candidate

New, explicitly selected headers only: `graph_band_index_candidate.h` builds an
index; `graph_index_band_candidate.h` provides full-frame preflight and absolute-y
band rendering. No CMake, main, Pet app or default backend changes are made here.

The existing six-pose fixed diagnostic scans about 550 packets at 32400 pixels
in two collector passes, twice for preflight and replay: roughly 71 million packet
visits per displayed frame. The candidate validates every packet's vertices,
deformed-local coordinates and finite area during index construction. Each packet
is placed in all intersecting eight-row bands in ORIGINAL packet order. Integer
sample AABBs are conservative and include boundaries. An X/Y check skips only
pixels where the existing coverage test necessarily returns no fragment.

Covered fragments still run the full existing evaluator in both collector passes,
including fragments behind exact opaque coverage. Invalid fragment rejection,
exact-alpha opaque handling, depth/order/index tie semantics, four retained-layer
limit, span limit and complete preflight are preserved. Geometry rejected before
index construction is not silently classified offscreen. This is validation
amortization under immutable-frame ownership, not permission to skip invalid data.

## API and ownership

```
hone_graph_bounds bounds[551];
uint16_t ids[1024];
hone_graph_band_index index;
/* payload is already in its final stable address; not pinned or DMA-borrowed. */
if (!hone_graph_index_build(&index, &payload.graph, revision,
                           bounds, 551, ids, 1024)) reject_frame();
/* Freeze payload, programs, driver values, bounds, IDs and index. */
hone_graph_index_band_preflight(&opaque, &payload, &index, revision, &work);
hone_graph_index_band_render(&opaque, &payload, &index, revision,
                            &work, y, rows, pixels);
```

These capacities fit the tested six poses, NOT arbitrary model/pose admission.
Capacity exhaustion fails before writing the descriptor or its arrays. Caller
supplies exclusive pending index storage and must not overwrite a borrowed/READY
index. The graph pointer identity, packet/driver descriptor, owner, generation and
explicit frame revision must match; work binding/generation is also checked.
Revision must change for a new frame even if pointer/counts do not. Pointer checks
cannot detect in-place mutation: source values and every dependency are immutable
through preflight, replay, physical completion and subsequent READY redraw.
Source descriptor relocation requires rebuilding after relocation. Maintain a
second index for a separate READY frame or rebuild only when no reader borrows it.

Build rejects overlaps between writable index spans and graph/driver/program
storage, and rejects overlaps between its own outputs. Band validation rejects
row/depth/scratch/pixel overlap with index storage as well as existing graph input
aliases. Trusted native caller contracts still apply to pointer validity and
concurrent mutation. A token alone does not establish physical completion.

## Evidence

Windows O0/O2 unrestricted-reference tests pass: 4000 scenes, 55140 equal pixels,
8860 retained-overflow rejections; ties, near-one alpha, late hidden invalid shader,
unchanged output on failure, invalid offscreen geometry, capacity failure and wrong
revision. Additional checks cover integer endpoints and rows 7/8, 127/128, 134,
plus band 1/2/4/8 pixel parity, nonlinear clipping, span, binding and alias failures.

All six private static model poses have exact full-RGB565 equality with the old
occlusion backend, not merely matching hashes. The previous dynamic-provider
hashes also match. Maximum index needs: 551 bounds (4408 bytes), 953 uint16 IDs
(1906 bytes). Descriptor-inclusive used memory on the 64-bit host is 6578 bytes;
allocated capacity can exceed usage, and target layout must be measured.
Worst-case ID capacity for arbitrary inputs is 17 times packet count.

Measured packet visits per preflight+replay fall from approximately 71.4 million
to 7.31 million; AABB-passing fragment calls are approximately 95,500. Windows O2
host preflight+replay changes from 1215–1249 ms to 9–11 ms, excluding index build.
These are local host timings, not ESP32 throughput or a 20 FPS result. Validation
at build remains mandatory for every changed geometry frame.

```
CC=cc python3 tools/hone/native_geometry/test_graph_band_index.py --out /tmp/band-index --sanitize
CC=cc python3 tools/hone/native_geometry/test_graph_index_band.py --out /tmp/index-band --sanitize
CC=cc python3 ../preserved/graph-index-static-v2/verify.private.py /tmp/index-six --sanitize
```

Private six-pose data remains outside the public tree. Lighting, near clipping,
Blender image parity, physical display/DMA and target RAM/stack/FPS are unchanged
unresolved gates. The fixed fixture performs no dynamic deformation on device.

A second host run averaged 0.061–0.106 ms per index build over 1000 repetitions per pose. Concurrent host load changed old render timing to 2.46–3.24 s and indexed rendering to 14–42 ms; treat these as workload evidence, not stable latency predictions. Both runs preserved all pixels.
