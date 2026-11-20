# Compact projected snapshot candidate

`projected_snapshot_candidate.h` is an opt-in, SDK-free producer. A packet is
exactly 44 bytes: nine vertex-major projected XYZ floats, material index and
original draw order (two uint32 values). An admitted material is 16 bytes.
`projected_sample_candidate.h` is a direct, deliberately slow reference consumer;
the separate compact raster candidate consumes the same packet layout.

Admission requires exact alpha-one and three single-CONST RGB programs, finite
RGB in [0,1], and original/clipped local components bounded by 1e20. The producer
checks the actual programs against the material table before dropping local
attributes. Unsupported materials require a separate full-attribute route or a
rejected frame. No alpha approximation, geometry deletion or color clamping is
performed. Viewport clipping preserves the existing triangle order and depth.

The caller supplies separate packet and material storage. `begin` requires an
increasing revision; append stages one clipped triangle group. Capacity or
validation failure preserves the prior appended prefix but poisons seal. This
single-buffer API does not retain a previous READY frame after begin. After seal,
pin with matching owner/generation/revision. Keep packets, table and derived
index immutable through LCD/DMA completion. `finish(..., false)` retains the pin;
only an authoritative external drain permits `finish(..., true)`. The bool/token
is a caller contract, not evidence that hardware completed. Use one exclusive
task, not concurrent calls or casts that bypass const ownership.

`test_projected_snapshot.py --out <fresh-directory>` checks O0/O2 packet layout,
clipping, all-panel RGB565 equality, admission, capacity/order overflow rollback,
and retained pins. Add `--sanitize` on a supported host toolchain. Final Windows
O0/O2 runs passed, including FLT_MAX local rejection.

An independent private host comparison evaluated all 63 sampled control poses:
runtime-only morph/rig/wrap, one-mesh lazy normals and direct attributed emission
produced byte-identical projected packet streams and all 2,041,200 RGB565 pixels
to the ordinary/general reference. Maximum 552 packets require 24,288 bytes;
maximum eight-row index memberships were 961. The private single-frame adapter
also matched all pixels and tested uncertain-drain retention and next-prepare
rejection. These are host sampled-pose results, not arbitrary-pose admission or
physical display/FPS evidence. Full material/lighting and near-plane clipping
remain outside this selective diagnostic route.
