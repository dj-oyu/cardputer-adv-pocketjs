# Native alpha packet candidate

`alpha_packet_candidate.h` is an optional host candidate lane, not a change to
production opaque packets. Each packet owns triangle xy/depth/local-mask values,
linear RGB, continuous base alpha, explicit mask bounds/enabled bit, stable order
and node identity. Near/far clipping preserves those attributes; a triangle emits
at most three triangles. No source pointer survives append.

Append validates and measures the entire immutable input before copying clipped
packets. Capacity failure, bad late input, generation/owner mismatch and aliasing
leave the candidate count/data unchanged. The candidate and ready buffers are
separate; publishing swaps descriptors only, rejects pinned buffers and retains
the old visible snapshot on failure. Caller exclusively owns all spans during
append/publish and must coordinate this lane with the opaque candidate: publish
both only after both succeed. The helper is not thread/ISR safe.

The existing opaque renderer and its packet ABI are untouched. These alpha
packets must NEVER be sent to its opaque depth-writing path. A production alpha
raster still needs shared opaque depth, stable per-pixel transparent ordering,
bounded slot/span preflight before LCD output, and immutable replay from the
same frame owner. Attribute storage/clip/transaction success alone does not
establish alpha rendering or full-character runtime admission. Actual material
graphs beyond the mask subset remain separate, as do normal precision gates.

Tests: compile `test_alpha_packet.c` at O2 and ASan/UBSan (leak checks separate),
and run `test_alpha_clip_native.py` for 1,000 random synthetic comparisons against
the independent Python clip reference. No private geometry is included. Memory,
clip arithmetic and renderer cost require target measurements before adoption.

## Native raster checkpoint

`alpha_raster_candidate.h` consumes the owned packet lane against immutable
opaque depth and existing RGB565 pixels. It implements top-left triangle coverage,
interpolated depth/local mask, continuous source-over alpha, far-to-near ordering,
four-fragment capacity and row-span rejection. Full-frame validation/counting
precedes writes; immutable replay supports identical 1/4/8-row results. Empty
alpha lanes return without scanning opaque pixels or depth. Float alpha is not
silently quantized to eight bits; final linear RGB is rounded to RGB565.

This checkpoint requires a full immutable opaque-depth input. That is a HOST
integration interface, not a valid claim that target memory can provide it.
The current production renderer only exposes row scratch; sharing that scratch
requires a whole-frame preflight/replay owner that covers BOTH lanes before any
LCD transfer. No LCD submission or two-lane publish implementation is present.
Do not enable runtime admission until that work and device budgets are verified.
`test_alpha_raster.c` checks band identity, continuous blend, foreground rejection,
late slot/depth failure with unchanged output, alias refusal and retry. Its tiny
synthetic timing is not whole-scene performance or target latency.
