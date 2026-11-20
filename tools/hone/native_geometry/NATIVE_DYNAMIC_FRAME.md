# Caller-owned dynamic geometry frame arena

This independent candidate is the first runtime-oriented boundary around the
verified host provider. It does not change firmware registration, main, fixed-pose
probes, CMake or Kconfig. Generated `hone_input_mesh` and provider node tables remain
immutable borrowed cold data. No model data is embedded in this header.

`hone_dynamic_frame_bytes` computes one arena containing provider descriptors,
all current source positions, one largest-mesh normal cache and one largest-mesh
double normal workspace. Allocation is external; init requires 8-byte alignment.
The state itself is separately owned, zero-initialized and never copied while live.
Normal arithmetic is supplied by a callback, so the same independently verified
polygon-normal implementation can be used without altering numerical semantics.

Lifecycle:

1. `init(state, owner, generation, cold, meshes, nodes, node_count, arena, capacity,
   normal_callback, callback_context)` binds the native owner and cold tables.
2. `begin(state, owner, generation, increasing_revision)` invalidates the normal
   cache, marks the frame mutable and initializes positions from source rest data.
3. `positions(state, owner, generation, mesh)` borrows writable current source XYZ.
   Apply admitted morph, rig, transforms and dependency-ordered wrap to these arrays.
4. `seal` checks every position is finite and starts the lazy normal provider for
   the exact frame revision. Mutable position borrows expire at this point.
5. `hone_dynamic_frame_provider` is the synchronous AYATSURI geometry callback.
   It computes normals on mesh switch, validates gather, then copies into the
   evaluator slot. Provider output remains unchanged on normal/gather failure.
6. After synchronous evaluation has copied geometry and local attributes into
   the pending snapshot/material payload, begin may reuse the geometry arena.
   READY/DMA-owned packet/payload storage must never alias this arena.

Owner/generation, monotonic revision, state, capacity, alignment and known aliases
are checked. Callback reentry/begin/dispose during provider execution is refused.
All other cold arrays and callback-owned data must remain disjoint from the arena;
this is a trusted native API, not arbitrary-pointer validation. A caller retaining
and mutating a position pointer after seal violates the contract and cannot be
made safe through a generation token alone. There is no implicit display ACK.

## Actual remaining dependencies

Cold candidates for flash: rest positions, topology/corner maps, source morph
arrays, weights, node partitions, driver programs/bindings, rig tables and material
programs. Dynamic work outside this API: driver controls/results, morph weights,
rig palettes/transforms, wrap target preparation and acceleration, scene targets,
stream instance, two immutable material payloads and display/index scratch.
Wrap target dependencies must be produced before source meshes consume them.
The arena is not a complete model RAM budget or a deformation evaluator.

Restored-data host arena is 39552 bytes: provider descriptors 1800, positions 15912,
normal cache 16344 and normal double work 5496. The descriptor size is ABI-specific;
the scalar payload sizes are not target allocation measurements. This excludes
all dependencies listed above and the 27496-byte host stream instance. The old
337140-byte ordinary reference instance is only an oracle and must not be copied
into the target integration. Two whole geometry arenas are unnecessary once all
provider attributes have been copied; two retained display payloads remain needed.

## Verification

Windows GCC O0/O2 synthetic 24-frame tests compare every provider output with the
retained implementation and exercise owner/generation, monotonic revision, failed
finite seal, unsealed/provider refusal, slot/arena overlap, capacity and disposal.
The existing lazy-normal cache and failure-slot tests run in the same executable.

Private rest plus 63-pose probe copies the existing live morph/rig/wrap results
into the new arena, seals and compares every provider slot exactly. All 64
streaming frames, packet/raster bytes, 256 rollback cases and READY after arena
release pass. This initial audit deliberately retains the old geometry/normal
arrays as oracle; it is not reduced process-heap evidence. Production can replace
that copy by writing deformation output directly into the begin/positions arrays.
The audit recognizes the ordered evaluator's first mesh to begin each test pass;
a real caller must use an explicit frame boundary rather than infer callback order.

```
CC=cc python3 tools/hone/native_geometry/test_native_dynamic_frame.py --out /tmp/dynamic-frame --sanitize
CC=cc python3 ../preserved/normal-full-diagnosis/run-dynamic-arena.private.py /tmp/dynamic-arena-real --sanitize
```

Target linker placement, heap/stack, dynamic-frame timing, physical DMA and 20 FPS
remain broker/device gates. Lighting, near clipping, complete source-package
admission and arbitrary-pose transform guarantees are not supplied by this API.
