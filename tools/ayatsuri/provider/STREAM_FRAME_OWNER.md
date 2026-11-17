# Streaming snapshot ownership candidate

`main/ayatsuri/ayatsuri_stream_frame_owner_candidate.h` supplies an explicit,
allocation-free bridge around the actual streaming producer. It does not register
an actor, load a model, change the default renderer, submit LCD work or enable a
Kconfig option. It is currently an opaque snapshot integration candidate, not an
execution-ready model or material renderer.

## Two buffers, one authoritative completion

Initialize a zeroed owner with an initialized streaming instance, a nonzero
generation, and two exclusive, disjoint packet allocations. Do not copy a live
owner. Never reuse its generation at the same address. The wrapper checks both
allocations against its descriptor, the streaming instance/arena and model/blob.
The borrowed instance/model and allocations must keep their identities and sizes.
Do not dispose/reinitialize the instance while production can occur.

1. `produce` writes only the noncommitted allocation. Provider failure invalidates
   that attempt and preserves the committed snapshot. A subsequent attempt fully
   recomputes it. No allocation or additional packet copy is introduced.
2. `acquire` calls a synchronous trusted preflight and pins the candidate only
   after it succeeds. The callback must check the **whole composed frame** before
   any display submission; it may not retain/mutate the snapshot or reenter.
3. A matching token borrows the pinned snapshot. All generation and acquisition
   attempts are rejected while pinned. The old committed snapshot remains intact.
4. `complete(..., drained=false, ...)` does nothing. Only the existing display
   owner's completion evidence may establish `drained=true`. Accepted completion
   promotes the candidate; failed presentation after drain keeps the old frame.
   Display repair remains the existing owner's responsibility.

Tokens correlate an acquisition, owner lifetime and revision. **They do not prove
DMA completion.** The firmware adapter must route its real `finish_display` /
presentation completion result here, preserving no-damage completion ordering.
An unknown/undrained transfer keeps both snapshots unavailable for reuse, even
when a submission failed. Pixel DMA bands have their own independent ownership;
this wrapper neither releases nor manages them. No arbitrary caller-supplied
boolean is acceptable as physical evidence in the eventual firmware integration.

The committed borrow expires at the next accepted completion. Additional readers
must not outlive that boundary; this is an exclusive synchronous native owner,
not a multithreaded refcount, ISR API or native-handle security boundary.
Stopping at a drained/unpinned boundary permits disposal of the producer while
keeping the committed packets for rendering; production must then cease.

## Memory and performance consequences

The wrapper adds 144 bytes on the tested 64-bit host and no heap allocations.
Target `sizeof`, placement and stack usage remain unmeasured. It retains exactly
two packet capacities, not a third frame. With the earlier private host maximum
of 35,280 bytes per snapshot, that is 70,560 packet bytes; it does not reduce the
190,432-byte subtotal documented in STREAM_CANDIDATE.md or account for driver,
material, world, wrap, bands, task stacks or other firmware activity.

GCC 15.2.0 Windows O2 reports 304 bytes for the specialized owner initializer
and 96 bytes for production wrapper in this host fixture. These are individual
host function frames, not target stack bounds or complete provider call chains.
The test deliberately keeps its synthetic buffers on its host main stack; that
test allocation pattern is not a firmware placement recommendation.

Production is deliberately serialized against display borrowing. This avoids
requiring a third snapshot to preserve rollback while DMA is pending. It can
limit overlap, so the frame budget must include geometry + whole-frame preflight
+ raster + actual display completion. It is not evidence of a 20 FPS frame time.
Revision exhaustion fails closed; it cannot silently recycle a stale token.

## Host validation and remaining integration

Run `CC=cc python tools/ayatsuri/provider/test_stream_frame_owner.py --out <new-dir>`
or add `--sanitize` for Linux ASan/UBSan. O0 and O2 compile the same actual
streaming API and raster sources. Each run exercises 32 synthetic frames and 64
rollbacks: provider/preflight failure, reentry, overlapping storage/slot,
wrong generation, stale/repeated completion, undrained transfer, display rejection,
successful promotion, revision exhaustion and ready rendering after arena release.
The preflight rasterizes every synthetic row. Display completion is simulated.

The separate 63-pose private geometry result uses sampled rig transforms and
placeholder materials; this ownership test does not extend that result to new
controls or real materials. A real material/alpha lane must be admitted, frozen
and preflighted atomically with its opaque snapshot under the authoritative frame
owner, including clipping/interpolation and overflow policy. Merely accepting a
material callback or token is insufficient. The default pocket/Kasane owner must
remain authoritative when the adapter is connected. Physical memory fit, task
high-water marks, LCD/DMA lifetime and target-model FPS require broker builds and
device validation; none is claimed here.
