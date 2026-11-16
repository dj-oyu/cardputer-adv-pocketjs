# Fragment-order experiment (not a runtime path)

Compile `alpha_slots_fixture.c` with `cc -O2 -std=c11 -lm`, or with ASan/UBSan.
It tests depth-crossing fragments, exact ties (first submission on top), opaque
occlusion, alpha0/255, band partition identity, late overflow and retry.
It does not rasterize triangles, produce packets, integrate Model or drive LCD.

An immutable preflight then deterministic replay avoids a fullframe candidate
copy. That immutability and ownership must be established in a real caller;
otherwise second-pass failure can expose a partial image. Opaque production
code is unchanged. Preflight currently repeats color work unnecessarily.

At240pixels wide, four12-byte fragment slots/pixel plus counts require11,760B;
RGB row adds480B, opaque depth adds960B if not reused. This is not just11.5KiB
of total memory, and hardware availability has not been established. Eight-row
band grouping still processes one row at a time, not eight slot rows.

Host synthetic fragment-stage comparison is diagnostic only. It excludes
triangle raster, packet decoding, actual geometry, LCD/DMA and MCU cost. Do not
infer whole-renderer overhead or20fps feasibility from it. Capacity4 is a test
limit, not a proved bound for any real asset. Overflow must never silently drop
fragments. Color-space/rounding compatibility and shader equivalence remain open.
