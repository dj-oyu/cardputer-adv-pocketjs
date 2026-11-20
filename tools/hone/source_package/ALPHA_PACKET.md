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

## Row-scratch frame owner checkpoint

`frame_lane_candidate.h` shares actual `ksn_mesh_render` one-row depth output
with alpha replay. It needs existing band pixels plus 240 depth floats (960 bytes),
not a full-frame buffer or separate row-color buffer. A frame descriptor binds
both immutable snapshots, owner and generation. Preflight validates opaque packet
topology WITHOUT opaque rasterization, then guarantees transparent capacity before
opaque occlusion. Occlusion can only reduce fragment count, so this is safe but
may reject a scene that would fit only after hidden fragments are removed.

The earlier opaque-raster preflight design doubled host opaque work and was
rejected. This candidate does one opaque raster pass. Transparent geometry still
gets capacity preflight and replay; its cost must be measured on the real scene.
Do not call the small synthetic alpha fixture a whole-character benchmark.

Full-frame preflight completes before descriptor commit. Pinned frame snapshots
remain alive until final band ACK; commit rejects a pinned old frame. Caller must
retain every referenced snapshot and packet buffer. No callback or allocator is
added. Existing opaque writes and alpha blends use the caller's band directly.

The existing LCD scheduler must call preflight before its first submission,
commit once and hold the owner pin through all bands. This candidate does NOT
install a production LCD hook by itself. `test_frame_lane.py` runs the real opaque
renderer with late alpha overflow, retained old descriptor, pinned commit rejection
and band comparisons, and records legacy/preflight/replay host timings separately.
An empty-alpha production frame can keep the existing opaque path unchanged.

Device stack/time/heap, actual LCD lifecycle and full-character alpha boundary
comparison remain required. Host stack high-water includes the entire test worker,
not isolated helper stack or ESP32 stack usage. Public tests are synthetic.


Independent review found and reproduced missing depth-scratch alias rejection,
uncoupled alpha/frame pin state, and reversed alpha boundary ownership. Candidate
repairs add explicit scratch/source alias refusal, coupled pin/unpin APIs and
actual opaque-renderer edge parity tests. Publication increments an alpha revision;
preflight captures it, and commit/pin/replay reject stale revisions even when the
pose generation is unchanged or buffers return to their previous addresses.
`hone_frame_preflight_pin` is the preferred synchronous entry. Pinning blocks
legal alpha publication until ACK/drain unpins. Opaque pointer/size changes also
reject. These are lifetime/identity checks, not a thread-safe lock: production
scheduler ownership must prevent concurrent mutation and retain source storage.
`test_frame_lane_review.c` contains independent alias, same-generation republish,
ABA and shared-edge reproductions. No LCD enablement follows from these repairs.
