# Procedural image workspace reuse

Base: integration `53a74ba82e4ff97b4b7a047cc7b2868b7d847151` (firmware
behavior inherited from `a1a93afbae73ca582ddc163166275dc2de7c06c2`).

The procedural adapter reserves a 3,840-byte static eight-row RGB565 image
cache and allocates a 10,246-byte plan execution scratch frame. These have
separate lifetimes on the synchronous UI owner task. A plan runs to completion
and its segments are copied into the surface candidate before image rendering;
the rasterizer reads candidate/committed frames, never that scratch frame.
Neither VM execution nor rasterization calls JavaScript or the other phase.

Use a union for the existing allocated workspace: one full-size frame or the
same eight-row image cache. A static assertion rejects workspace growth.
Candidate/committed frames, VM limits, segment copies, image cache dimensions,
TX ownership, dirty windows and resource thresholds are unchanged. Each VM run
invalidates the cache *before* writing, including failures and abandoned builds.
Image reads during a later build can therefore rerasterize the committed frame.
Alternating surfaces, partial spans and repeated/scaled rows retain their cache
key. Reset frees the workspace once; a frameless image fills the requested span
without allocation, including while allocations fail. Raster errors leave the
cache invalid. This sharing is local to this adapter and does not borrow the
board strip or DMA payload.

## Evidence and limits

Actual Xtensa object compilation using the saved ESP-IDF 6.0.1 build headers and
flags: `pocket_proc.o` BSS is 4,343 bytes before and 503 after, for **3,840 bytes
less static internal memory**, independently for DMA OFF and ON. This is an
object result, not a linked-image or device free-heap measurement. Allocation
hook tests retain four first-frame allocations with the same 10,246-byte scratch
size, and no allocation is added for image reads.

Existing real-QuickJS image tests compare 48 RGB565 frames, two independent
surfaces, damage, IO retry, reset and stale resources. Additional checks cover
workspace writes followed by abandoned/failed builds and re-reading the same
committed raster, including both surface orders. Existing ASan/UBSan scalar and
fake PIE limits/turn tests exercise these cases and allocation failure. Removing
the pre-VM cache invalidation makes the new committed-row comparison fail.
The usual complete CI entry remains the acceptance gate (ASan/O2, captures,
games/m32, baseline/current, source/tree/overlay identity and all-success gate).

The existing physical baseline has largest free block 14,848 < 16,384 bytes and
guest headroom approximately 24,140 < 32,768 bytes. This patch does not reduce
live guest allocations. It does not establish a new largest block, solve the
guest gate, or establish timing improvements. DMA OFF/ON require independent
physical evaluation after CI/review. No device update is included here.

## Remaining candidates

Guest retention needs separate object/bytecode/lifetime evidence; existing lazy
intrinsics, ROM atoms, lazy API namespaces, stripped function source and native
block-cache pressure release are already implemented. Reapplying them is not a
new saving. Turning off debug information changes diagnostics and is not part
of this patch. Native savings must not be added to guest-cap headroom.

The UI task reserves 32 KiB; an earlier physical snapshot reported 22,892 bytes
unused. Its guest stack limit is 20 KiB, and that snapshot is not a worst-case
native UI/error/parser stack proof. Leave both budgets unchanged until a
bounded stack analysis and native/error-path tests justify another budget.
Join any later candidate only after separate validation, then rerun the same
complete suite and independent OFF/ON acceptance. No thresholds are relaxed.
