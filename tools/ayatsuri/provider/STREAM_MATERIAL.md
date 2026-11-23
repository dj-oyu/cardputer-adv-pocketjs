# Frame-associated material payload and graph-band experiment

The default-OFF streaming actor now has a material-hook entry. Each of the frame
owner's two snapshot slots carries an optional immutable payload pointer. After
successful geometry production, `ayatsuri_stream_frame_payload` stages that
slot's payload before preflight/acquire. New production clears the pending
payload. ACK swaps geometry and payload together; rejected completion clears the
failed pending payload and preserves both old READY geometry and old material.
Staging is refused during production, pinned submission or READY redraw.

The caller owns the complete payload dependency graph: triangle attributes,
program instructions, driver values, material bindings and provenance metadata.
They must remain immutable while pending is borrowed, while completion is
uncertain, and for every subsequent READY redraw. Use two separately owned
material/driver arenas when updating a frame. A failed drain is not permission
to overwrite either arena. A promoted old payload may be reclaimed only once
the prior READY is no longer borrowed; successful unbind/reset ends actor borrows.
Raw native pointers and generation checks are not authentication or source hashes.

`kobako_kasane_stream_actor_bind_material` copies trusted backend callbacks into
the APP-bound registration. The backend receives the exact snapshot and payload
selected by the frame owner, for both preflight and each absolute-y band. It must
preflight the whole frame before first submission, fill every output pixel,
respect disjoint scratch/input storage and propagate evaluation failures. It
cannot execute script, mutate source/owner, submit display work or reenter.
Existing physical completion/rollback/recovery rules are unchanged.

## Working experimental backend

`tools/hone/native_geometry/alpha_graph_band_candidate.h` connects the retained
graph fragment evaluator to one-row opaque depth and caller-owned band output.
It uses deformed object-local XYZ carried through the existing orthographic
viewport clip, interpolates those coordinates per fragment, then evaluates
nonlinear alpha and three base-color programs. Alpha is not approximated by
interpolating vertex alpha, and local coordinates are not reconstructed from
screen positions. Stable depth/order sorting and the four-fragment overflow
contract are preserved. It uses 240 pixels + 240 depth floats + caller-sized
graph scratch, avoiding the prior full-screen depth allocation.

This backend is a host experiment and is not automatically linked into firmware
or registered. Its RGB is **unlit linear base color** composed into the existing
RGB565 reference convention. It does not implement Principled lighting, color
management, textures, perspective-correct attributes or camera/near clipping.
An opaque placeholder packet representing the same translucent surface would
incorrectly occlude its graph; the package builder must route those surfaces once,
not draw placeholder opaque geometry underneath the same material geometry.

## Verification

`test_alpha_graph_band.py --out <new-dir> [--sanitize]` compares every pixel with
the retained full-screen graph reference across 1/2/4/8-row calls and checks
clipped nonlinear local-coordinate alpha, alias rejection, package binding and
fragment/span overflow. Windows O0/O2 passed. `test_stream_display.py --sanitize`
also links the actual kobako/Kasane actor with this backend: distinct immutable
driver payloads, clipped graph triangles, old-material redraw after failed drain,
successful joint promotion and wrong-binding rejection before any send. These
are public synthetic fixtures, not a restored-model appearance result.

The frame owner grows from 144 to 160 bytes on the tested 64-bit host (two payload
pointers). Target size/placement remain build measurements. The graph backend
performs a full preflight and replay; it is deliberately a semantic reference,
not a demonstrated 50 ms implementation. It still scans all graph packets at
each pixel and executes coordinate-dependent programs per covered fragment.

## Concrete remaining real-model integration

1. Export/admit one provenance-bound package that agrees on the source hash,
   source polygon/material slots, partition/node IDs, driver indices and shader
   programs. The source package and inventory currently remain offline assets;
   this API loads neither and performs no asset SHA verification.
2. Build attributed triangles from the actual evaluated source geometry, retaining
   deformed local XYZ through every transform and clip. The compact opaque packet
   intentionally lacks those attributes. Existing viewport clipping alone does
   not establish camera/near-plane behavior.
3. Complete and validate actual shader/alpha/color/lighting semantics, route all
   surfaces without duplicate opaque occlusion, and prove full-scene layer/span
   limits without deleting geometry or weakening overflow checks.
4. Freeze material drivers with the geometry's controls, rig, wrap and normals in
   one frame; allocate both payload arenas and all scratch at bounded startup.
   Measure their combined target internal-RAM/stack cost.
5. Compare complete restored-model images and controls, then measure the exact
   target firmware/model's raster plus real display completion through the broker.

The earlier 63-pose geometry gate used sampled rig transforms and placeholder
materials. The real synthetic CPU raster measurement of about 325.807 ms/frame
also shows that ownership and semantic connectivity alone are insufficient for
20 FPS. Neither result is a completed real-model rendering gate.
