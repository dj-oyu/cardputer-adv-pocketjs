# Bounded row-span host experiment

`alpha_span_fixture.c` computes coverage spans from every candidate frame,
then validates all row capacities before output writes. It does not reuse a
camera/pose sample as a universal geometry guarantee. Width>54, coordinate
out-of-view, nonfinite input or >4surviving fragments rejects before mutation;
retry with a valid immutable frame succeeds.1/4/8 band grouping is identical.

Compile with `cc -O2 -std=c11 -Wall -Wextra -Werror -lm`; ASan/UBSan also work.
The input sample array is a synthetic substitute for replayable triangle raster
outputs, not a proposed fullframe runtime fragment buffer. Actual integration
must use owned immutable geometry through preflight/replay. The fixture has no
LCD, Model packet integration, concurrency or STOP handling.

Measured C layout: row work2,756B (slots/counts/RGB including padding), plus
8B bounds per row. At135rows that is1,080B metadata, and a separate240wide
opaque-z row is960B if needed:4,796B combined, not the earlier2,970B estimate
which omitted all-frame bounds and full-width depth workspace. Geometry input,
existing LCD buffers and stack overhead are additional. This is still not a
hardware allocation proof. Compact bounds or recomputing spans trades memory
against traversal, and needs another measured experiment.

Only the transparent experiment uses these structures. Existing opaque code
is unmodified; its host240frame/240pause raster regression passes. No reliable
same-condition whole-renderer timing comparison or device performance claim
is established by a fragment-only fixture.
