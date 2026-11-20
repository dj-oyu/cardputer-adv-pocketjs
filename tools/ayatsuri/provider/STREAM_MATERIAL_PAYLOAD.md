# Bounded material payload assembly experiment

This opt-in host candidate connects the typed attributed sink to frame-owner
production. The default stream producer, renderer, actor registration, firmware
configuration and assets are unchanged. Nothing automatically registers this
backend or supplies the restored model to firmware.

`stream_material_payload_candidate.h` admits a trusted immutable package with
one MATERIAL route per mesh, authored triangle to source polygon/material maps,
program pointers, expected source hash and driver binding/count. Every polygon
reference must select the declared material. The stream evaluator checks node,
mesh descriptor, cold material identity and complete ordered route coverage.
The wrapper copies current driver values and clipped projected/local attributes
into bounded caller-owned pending arenas. Programs remain borrowed immutable.
Source triangle ordinals select the source polygon even after visibility/depth
ordering. Hidden meshes emit no packets; no opaque placeholder is appended.

Viewport clipping uses the existing orthographic clip and carries deformed
object-local XYZ with the same edge interpolation as depth and screen position.
These are unlit graph attributes; camera/near clipping, lighting, color-management
and final image equivalence are NOT implemented here.

## Ownership

The new `ayatsuri_stream_frame_produce_attributed` entry increments revision and
stages geometry/payload together only after successful evaluation. Provider/sink
or capacity failure leaves private pending partial, never published; old READY is
unchanged. Both arenas remain unavailable while pinned or completion is uncertain.
ACK promotes both; rejected completion preserves old READY. The existing token
requires authoritative external display completion; test ACK is simulated.

Use this builder only with owners whose non-null payloads have the exact
`ay_material_pending` layout. It begins with the existing graph-band payload, so
a trusted backend can consume that first member. Never mix arbitrary payload
formats. Its wrapper rejects known aliases with owner/model, both snapshot
allocations, route/program inputs and all old READY packet/driver storage.
Caller provider/context-owned data must also stay disjoint and immutable. Native
pointers and callback contracts cannot enforce arbitrary malicious aliasing.
Two dedicated payload arenas and immutable program storage are required.

Drivers must be the same evaluated program outputs used to build this frame's
geometry; binding/count checks do not prove that a dishonest caller supplied the
right values or numerical program order. Export/admission must retain that
relationship. The private test uses the live `values` array from the exact same
63-pose driver evaluation as morph/rig/node/wrap geometry.

## Export and evidence

`export_material_payload_fixture.py` recomputes SHA256 over the actual supplied
source bytes and compares source/inventory/driver-route/scene identities. It
revalidates polygon material slots and shader driver socket routes, partitions
source triangles, and recompiles alpha/base-color programs from the same material
inventory/routes. Output is asset-derived and must remain PRIVATE. Hash equality
is byte identity, not authentication or independent proof of Blender export.
The private host test additionally matches every cold triangle index and node ID
to this exported partition before using the manifest.

Windows GCC O0/O2 synthetic tests pass: 16 frames, 48 rollback checks, copied
finite drivers, wrong source/polygon/binding rejection, READY alias rejection,
hidden meshes, capacity overflow, composed graph preflight, and freeze/ACK.
Existing frame-owner regression passes O0/O2: 32 frames/64 rollbacks. Python export
checks pass for changed source bytes, missing polygon use and duplicate partition.

Private restored-data test passes all 63 sampled poses with actual recompiled
alpha/base-color programs and copied same-frame driver values; maximum viewport
clipped packets 538. Its existing opaque reference comparison still passes
64 frames/256 rollback cases with exact packet/raster bytes. **Material raster
preflight/image comparison is not run on that model**: full-scene four-layer/span
admission remains a separate gate, as do sampled rig transform generality, physical
DMA, target RAM/stack and 20 FPS. Placeholder opaque reference and new material
payload results must not be conflated.

Run public tests from the worktree:

```
CC=cc python3 tools/ayatsuri/provider/test_stream_material_payload.py --out /tmp/material-payload --sanitize
CC=cc python3 tools/ayatsuri/provider/test_stream_frame_owner.py --out /tmp/material-owner --sanitize
python3 tools/ayatsuri/provider/test_export_material_payload.py
```

The private runner is preserved outside the repository under
`.cache/preserved/normal-full-diagnosis/run-material-payload.private.py`; it takes
a fresh output path and optional `--sanitize`. Source model and generated headers
are never embedded in public test files.

## Memory and cost limits

Packet capacity is explicit (at most 65536); capacity exhaustion rejects the
frame rather than dropping triangles. Driver capacity is also bounded. The
private host test deliberately allocates 12000 packets per slot as a diagnostic
ceiling, not a target budget; 538 is only the observed sampled maximum. Two
payload arenas add packet and driver memory beyond the existing stream arena and
opaque snapshots. On the Windows O2 host the observer reports 1840 bytes of
compiler stack frame and producer 560 bytes, excluding callers; O0 reports
864/528. These are NOT ESP32 stack measurements. Clip staging and graph replay
cost require target measurement and likely further work before real admission.
