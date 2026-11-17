# APP-bound native streaming actor candidate

The existing default-OFF `AYATSURI_STREAM_DISPLAY_CANDIDATE` option now also
offers `pocket_kasane_stream_actor_bind`, `unbind` and `bound`. No automatic boot
hook or data asset is added. The caller prepares the pocket APP lease, initializes
the streaming owner, produces an initial frame, and explicitly binds the owner,
identity/generation, depth scratch and immutable preflight context. Binding
activates that APP and refuses a second registration or a competing procedural
background/pending native publication.

Normal `pocket_kasane_present_complete` then uses the candidate when `produced`
is set, otherwise the committed READY snapshot. Producing a new frame makes
`pocket_kasane_needs_present` true, without a separate caller invalidation. UI
damage redraws the committed geometry through the actual pocket/Kasane renderer;
no-damage presentation performs completion without rasterizing a full frame.
READY redraw temporarily locks the frame owner against production. Failed drain
retains that lock, even though no new snapshot was being promoted.
The caller must use the completion-capable presentation entry; the legacy entry
without a completion callback cannot present a bound actor and fails closed.

The registration is tied to the exact APP lease. A successful ordinary reset
clears it only after the existing runtime detach succeeds. Reset refuses while
production, acquisition or an uncertain display borrow is outstanding. Explicit
unbind requires a real completion callback; failed/unknown drain retains both
registration and producer lock. Recovery drains only, never ACKs a failed candidate,
and invalidates for repair. Source/model/arena/scratch/context remain caller-owned
and must stay alive until `bound()` is false following successful unbind/reset.
The adapter never frees them. A stale APP lease cannot silently migrate the actor.

The normal dormancy path suppresses the actor backdrop while preserving its
registration and storage. A held display is recovered before presentation can
proceed. Alternative explicit backdrop entry points refuse to bypass a registered
actor. Callbacks and all native APIs remain exclusive to the synchronous UI owner;
no script/reentry, external mutation of frame metadata, or concurrent source use
is admitted. This is not a security boundary for forged native handles.

## Scope and provenance

This API accepts a caller-supplied streaming frame owner. It does not load the
restored Blend/source package, compile a model, resolve input bindings, evaluate
controls/driver dependencies or advance animation. No actual material or alpha
pipeline is installed: pixels are the admitted opaque RGB565 packet rendering on
black. A real-model caller must supply a provenance-bound immutable package and
freeze all controls/geometry/material bindings for the entire frame. Placeholder
materials and the sampled rig transforms from the 63-pose host gate must not be
reported as complete real-material/arbitrary-control support.

Memory remains two caller-sized snapshots plus geometry/arena, 240-float depth
scratch and a small static registration record. No per-frame allocation or third
snapshot is introduced. Target structure sizes, placement, combined internal-RAM
fit and task high-water margins are not established by this API. Target timing
must include whole-frame geometry, preflight, raster and physical completion.

The measured CPU-only synthetic device probe reported about 5.226 ms geometry
and 325.807 ms raster per frame for its three 32-vertex meshes, without DMA. This
is a serious raster-budget failure for that workload, not a target-model FPS
result. Registration/ownership correctness does not solve it. The reference
raster repeatedly decodes and validates packets and reconstructs triangles for
each scratch row; its double-precision edge setup/division is also repeated for
off-row triangles. These are code-derived optimization candidates, not yet
measured cost attribution or a speedup claim.

## Host verification

`test_stream_display.py --sanitize --out <new-dir>` retains the actual pocket
regression harness and adds APP binding, normal-entry frame presentation, READY
repaint pixel equality, no-damage completion, redraw drain failure, refusal of
reset/unbind/production while borrowed, recovery, explicit unbind and reset
clearing registration. The synthetic copied-DMA port exercises callback ordering;
physical device validation remains a separate broker operation.
