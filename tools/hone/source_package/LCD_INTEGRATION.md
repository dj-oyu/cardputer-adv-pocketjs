# Pending-frame to LCD integration plan

Current verified production integration points are in `main/pocket/pocket_shiki_frame.c`.
This document is a wiring plan, not an assertion that the hook is enabled.

1. Keep alpha candidate/committed packet owners alongside the existing depth
   candidate/committed snapshots in `proc_surface`. Preserve existing empty-alpha
   path without new preflight or renderer calls. Native append must retain all
   source attributes or reject; no JS registration or opaque-material fallback.
2. In `commit_impl`, use the coupled `hone_frame_preflight_pin` operation on the complete candidate before
   `pocket_kasane_proc_publish[_at]`. A failure leaves committed data and LCD
   untouched. Preparing a descriptor is NOT a displayed-frame commit. A failed UI publish must unpin and discard only the pending candidate. After
   successful publish, freeze the paired painter/depth/alpha candidate and pin
   the complete pending frame before any image/backdrop row callback.
3. In `render_rows`, retain `render_painter_rows`, then choose the paired
   candidate or committed lane with the same existing frame-pointer selection.
   Share existing `depth_workspace` for row replay. Do not render opaque twice.
   `image_rows`, span fallback and backdrop must see the SAME frame identity;
   changing the band size must not rebuild, swap or mutate packet inputs.
4. Only `pocket_proc_present_result(KSN_OK)` promotes candidate to committed.
   Swap depth and alpha owners together with the painter frame, then unpin after
   the real final ACK. Descriptor publication helpers must not promote the
   externally visible committed identity before this point.
5. On image-mode `KSN_IO`, retain the exact pending candidate and its pins for
   the submitted resource/ticket retry. On backdrop IO, follow existing old-frame
   repair semantics; release candidate only after outstanding loans drain.
   STOP/reset must drain or explicitly invalidate every owner, not free packets
   referenced by DMA or a retry ticket. Failure after partial physical transfer
   is repair/retry, not a promise of physically atomic LCD contents.

Required integration tests: publish failure before first write, 8-row late IO
then 4-row retry with identical identity, image versus backdrop repair, STOP while
loaned, old-frame rendering after rejected candidate, alpha/depth scratch aliases,
pin coupling, first/last shared-edge coverage and no warm allocation. Keep host
mocked-LCD evidence distinct from actual device DMA/ACK, FPS, stack and heap.

Current candidate tests validate paired descriptor and raster contracts only.
Production surface fields, native registration, LCD callback hook and actual
loan/ACK ownership are not implemented by the helper headers. Independent review
and device measurements remain mandatory before runtime admission.

The exclusive owner interval starts before validation and ends only after the
final ACK/drain. Same-generation alpha republish is detected by the snapshot
revision, including two-publish ABA. Coupled pin blocks alpha publish. This is
not sufficient against unsynchronized raw memory writes or freeing opaque
buffers: the actual scheduler must enforce those ownership boundaries. Until
that wiring is implemented and reviewed, real LCD admission remains closed.
