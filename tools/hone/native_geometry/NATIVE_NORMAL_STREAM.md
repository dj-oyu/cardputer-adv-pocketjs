# Lazy normal provider for dynamic-frame integration

`native_normal_stream_candidate.h` is an independent provider, not a firmware
switch. It separates immutable generated topology/morph/weight/node data from
frame-local deformed positions and a reusable normal cache. The caller supplies
the existing full-polygon normal computation callback and one bounded normal
buffer. A mesh switch computes normals once; its subsequent partitions reuse the
same buffer. Out-of-order revisits recompute rather than returning stale normals.

`hone_normal_stream_begin(state, monotonic_frame_id)` MUST run after all driver,
morph, rig, transform and wrap work is complete and before any provider use for
that frame. It invalidates the cache even when controls are numerically unchanged.
Positions, node mapping and cold mesh data stay immutable until all partitions
have copied their attributes into pending packets. No normal pointer escapes this
provider. READY/pinned packet and material payload lifetimes remain unchanged.
Frame IDs must not wrap or be reused; replace the state at a safe owner boundary.

The source context's `packed_normals` field is unused by this provider; a dynamic
caller can omit full-scene retained normal arrays. The callback must fill all
polygon corners for its input mesh, without aliasing model/positions/slot/snapshot
storage, and must not reenter. Callback failure invalidates the cache and leaves
the provider output slot unchanged. Gather validates all indices/normals before
writing any slot. Capacity and cache/slot overlap are checked. This is trusted
native code, not arbitrary-pointer admission or a display scheduler.

## Memory audit

Restored-source aggregate scalar counts (host-independent float payloads):

| Data | Bytes | Lifetime |
| --- | ---: | --- |
| All deformed source positions | 15,912 | Current geometry frame |
| All retained polygon corner normals | 70,968 | Old host implementation |
| Largest mesh normal cache | 16,344 | Reused during provider evaluation |
| Largest normal double scratch | 5,496 | Reused by normal callback |
| Old dense all-one morph masks | 2,024 | Existing diagnostic allocation |

The normal-cache substitution removes 54,624 bytes of float payload from the
prospective dynamic runtime, assuming full-scene normals have no other consumers.
This is a calculated layout reduction, not measured ESP32 heap recovery. The
normal calculation still uses the same arithmetic; it is not an approximate
normal method. It was prioritized over a graph hot-path micro-optimization
because internal RAM is an immediate blocker for dynamic model integration.

Generated rest positions, topology/corner mapping, shape keys, weights, driver
programs, rig tables and graph programs are immutable candidates for flash-backed
storage. They are not all disposable: wrap target geometry and same-frame target
transforms must remain available until dependent wraps complete. The source
positions above, rig/wrap work, driver values, source state and two material payload
arenas still require a complete runtime memory budget. The ordinary 337140-byte
host reference instance must not be carried into the target; its separate stream
instance is 27496 bytes on the tested host. Target linker placement/stack remain
broker-build measurements.

## Verification and scope

Windows GCC O0/O2 synthetic test: 24 changing frames, exact provider slot equality
with retained normals, one calculation per mesh across multiple partitions,
callback failure and invalid final gather index preserve the entire slot,
capacity/alias rejection, monotonic frame invalidation and reentry rejection.

Private host rest + 63 poses also pass exact provider slot comparison, 64 streaming
frames, packet/raster byte equality, 256 rollback cases and READY after source
free. The private audit keeps the old arrays only as an oracle; it demonstrates
substitution correctness, not reduced process heap. It explicitly detects the
ordered test evaluator's first mesh to begin each audit pass. Production must call
`begin` at its own frame boundary rather than infer it from callback order.

```
CC=cc python3 tools/hone/native_geometry/test_native_normal_stream.py --out /tmp/normal-stream --sanitize
CC=cc python3 ../preserved/normal-full-diagnosis/run-lazy-normal.private.py /tmp/normal-stream-real --sanitize
```

No fixed-pose probe, CMake, Kconfig, firmware asset or device operation changes.
Dynamic source-package admission, target allocation, lighting, near clipping,
physical DMA and 20 FPS remain separate unresolved gates.
