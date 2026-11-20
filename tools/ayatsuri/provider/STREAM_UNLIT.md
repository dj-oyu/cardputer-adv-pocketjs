# Positions-only unlit streaming candidate

`ayatsuri_stream_candidate_frame_attributed_unlit` is a separate opt-in API.
The typed provider writes only `ayatsuri_stream_position.position[3]`. Every
component must be finite and within [-4096,4096]; untouched provider fields are
initialized to NaN and rejected. The usual owner, generation, disjoint pending
storage, exact model identity and complete ordered mesh-route checks apply.
Missing sink, missing routes, OPAQUE or mixed routes are rejected before provider
invocation or pending reset. Hidden meshes also require MATERIAL routes.

The caller must admit its unlit constant-opaque material programs and source
provenance separately before publishing. This evaluator does not inspect shader
programs or authenticate matching hash strings. It projects current geometry and
preserves winding, visibility, original triangle IDs and depth ordering. It skips
normal validation and discarded Gouraud shading only on this explicit API.
The attributed sink sees NaN normal sentinels and zero Gouraud colors; it must
consume positions/projected coordinates/depth only. No fabricated unit normal
is used. Ordinary and existing attributed APIs still validate normals and shade.

No model wire format, public instance layout, initialization or persistent frame
ownership changes. The ordinary model TU remains separately compiled; the flag
and positions-provider bridge exist only in the internal stream request. The
bridge adds 384 bytes of bounded stack scratch (32 XYZ results), not a retained
per-mesh allocation. It validates all positions before copying to the old slot.
Normal storage savings depend on a future positions-only producer allocation;
the current diagnostic still reserves its original normal workspace.

Failure can leave unpublished candidate/pending work partial. Separate READY
storage stays owned by its caller. This API neither publishes nor pins DMA and
does not permit mutation of an in-flight material snapshot. Lighting and missing
near-plane clipping are not implemented by this capability.

`test_stream_unlit.py --out <fresh> [--opt 0|2] [--sanitize]` retains strict
stream/ordinary packet and raster comparisons, invalid-normal rejection and
READY rollback; it adds strict-versus-unlit XYZ/projected/depth/topology equality,
NaN normals, zero unused colors, mixed/no-sink rejection and invalid XYZ/sink
failures. Windows O0/O2 each passed all three fixtures (24 frames each).
The private live producer also passed all 63 sampled control poses with all
2,041,200 RGB565 pixels identical to the independent ordinary/general reference.
These host tests do not establish physical target memory, DMA or frame rate.
