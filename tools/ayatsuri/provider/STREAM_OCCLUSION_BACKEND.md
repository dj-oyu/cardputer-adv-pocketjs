# Explicit occlusion material backend and actor integration

`stream_graph_occlusion_backend.h` constructs opt-in material hooks for the
existing kobako streaming actor. It calls the explicitly named occlusion band
preflight/render functions; no macro substitution, default backend change,
firmware auto-registration or asset installation occurs. The old band backend
remains usable in the same translation unit.

The backend consumes the exact immutable graph-band payload associated with the
frame-owner snapshot. A bounded `ay_material_pending` may supply its initial
band member. Driver values, alpha/base-color code and clipped deformed-local XYZ
are part of that frame. Use existing frame-owner production/staging, full-frame
preflight, physical completion and joint ACK. Scratch/work must remain exclusively
owned until actor unbind has confirmed completion; failed/uncertain completion
keeps all borrows. The hook constructor itself does not authenticate data or
prove physical DMA completion.

`alpha_graph_occlusion_band_candidate.h` preserves the previous band validation,
absolute row coordinates, span limit and four retained-layer bound, selecting
only the separately tested exact-opaque collector. Every fragment remains subject
to evaluation and invalid rejection. Preflight must cover the full viewport before
any send; failure in replay can leave private output rows partial and follows the
actor's existing drain/rollback/repair rules.

## Host checks

The explicit band backend passes Windows O0/O2 pixel comparison at 1/2/4/8 rows,
clipped nonlinear attributes, overflow, binding and alias checks.

`test_stream_occlusion_display.py` uses the actual kobako/Kasane actor, synthetic
geometry and a copied-DMA host port. It adds five opaque background layers to the
clipped nonlinear foreground, compares every displayed pixel with unrestricted
composition, and exercises changed driver payloads, failed completion, old READY
redraw, rejected binding and an invalid shader hidden behind opaque coverage.
Parent independently ran the Linux ASan/UBSan synthetic actor suite: PASS,
0 failures. The initial build found a misleading-indentation warning in the new
fixture; the loop was braced and the final source was rerun successfully.

The private `run-material-actor-six.private.py` runner links the actual kobako
harness and the existing restored-model provider in separate translation units.
It evaluates the same source-derived 63-pose controls/rig/wrap pipeline, builds
bounded payloads from actual recompiled alpha/base-color programs and copied
same-frame driver outputs, then routes six representative poses through actor
registration/presentation/ACK. The expected panel is independently composed with
unlimited fragments. All model/program inputs remain outside the public tree.
The public `stream_graph_actor_probe.inc` contains only the generic host bridge.

Parent independently ran the private Linux ASan/UBSan actor probe successfully:
`REAL_GRAPH_ACTOR frames=6 full_panel_pixel_equal=true joint_payload_ACK=true
DMA=simulated lighting=false near_clip=false`. This establishes the tested host
actor/packet/payload association and copied-port completion, not physical DMA.
The accompanying provider run covers 63 poses; material actor display covers six.


Commands from the worktree:

```
CC=cc python3 tools/ayatsuri/provider/test_stream_occlusion_display.py --out /tmp/stream-occlusion-actor --sanitize
CC=cc python3 ../preserved/normal-full-diagnosis/run-material-actor-six.private.py /tmp/material-actor-six --sanitize
```

Both require the Linux host dependencies of `build_kasane_test.sh`. An existing
compatible QuickJS object cache can be supplied to the private runner through
`QJS_CACHE`; fresh test outputs remain mandatory.

## Remaining admission work

This is an opt-in host integration, not an installed executable model package.
The private fixture verifies source byte hash and triangle/material mapping, but
runtime packaging must preserve provider/program/driver identity, capability
admission and both payload arenas within the measured target budget. The graph
is unlit alpha/base-color; final lighting, camera/near clipping, color management,
Blender image parity and arbitrary-pose rig guarantees remain incomplete.
Host copied-DMA ACK is simulated; physical display, RAM/stack and 20 FPS require
the exact firmware/model through the broker. Two-pass fragment evaluation can
increase CPU cost and is not a demonstrated speed improvement.
