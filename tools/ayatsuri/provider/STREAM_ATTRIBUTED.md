# Typed attributed streaming candidate

Explicit native-only extension of the separately compiled streaming candidate.
The ordinary model API, default stream call, firmware configuration and display
actor are unchanged. No private assets are included.

`ayatsuri_stream_candidate_frame_attributed` validates an immutable route manifest
against exact model identity and every mesh node in ascending node order before
resetting pending or calling the provider. Node, descriptor and cold material IDs
must match; missing, duplicate, extra and invalid-lane routes fail closed.

The synchronous callback receives current deformed object-local XYZ/normal,
projected vertices, depth, selected indices and original cold triangle ordinals.
Indices refer to the full provider vertex array, not compact packet numbering.
Compact packets remove unused vertices; comparison must account for this remap.
Original triangle ordinals preserve visibility selection and stable centroid
ordering, including duplicate index triples. Hidden meshes still receive provider
validation but emit zero drawing triangles and null index/source-ID pointers.

All callback pointers expire when the callback returns. Copy needed values into
exclusive pending payload storage; never retain pointers, reenter, invoke script,
or mutate inputs. This trusted native API cannot police a callback which violates
its borrow contract. Route and model storage also remain immutable during the call.
OPAQUE routes append as before; MATERIAL routes skip opaque append entirely, so
one mesh is not drawn twice. A sink failure can leave private pending data partial;
separate READY storage remains owned and unchanged. No publication or DMA is
performed. Consumers must not write pending storage while pinned/undrained, and
must freeze geometry and material payload together through ACK and READY redraw.

This is a prerequisite for actor integration, not automatic integration. Still
required: source-package/program hashes, source polygon-to-material provenance,
bounded copied payload allocation, frame-owner production entry and composed
preflight before publication. Existing cold material IDs alone do not prove the
restored material graph mapping. Lighting, near-plane clipping, alpha admission,
actual material rendering and target memory/FPS remain unverified here.

## Host verification

`CC=cc python3 tools/ayatsuri/provider/test_stream_attributed.py --out /tmp/attributed --sanitize`
(add `--opt 0` for O0). Four synthetic fixtures each run 24 frames, packet/raster
comparison, hidden meshes, sink failure, invalid manifest checks and 1124 existing
provider/capacity rollback cases. The fourth has duplicate triples and centroid
ordering with varying vertex depth. MATERIAL routing yields no opaque packets.

Windows GCC O0 and O2 pass. The unchanged device-probe test also passed both
optimization levels and 1/8 depth rows with all 32 frame pixels identical.
Restored-model private host comparison passes rest plus 63 sampled control poses,
231 mesh providers per frame, 256 rollback cases, exact packet/raster equality and
READY rendering after source/model disposal. Materials remain placeholders; this
reuses the previously sampled rig transform and is not new Blender ground truth.
The first observer comparison incorrectly assumed compact packets kept unused
vertices; its remap was corrected before the successful private comparison.

No new persistent arena is allocated: private host instance remains 27496 bytes,
versus 337140 for ordinary all-mesh storage; maximum opaque snapshot is 35280.
These are host layout values, not physical target RAM. Per callback the mapping
uses 64 uint16 IDs, 64 bool flags and a view plus compiler frame overhead; matching
is bounded by 64 squared triple checks. Target stack/time is unmeasured, and a
future directly propagated ordinal could avoid this scan. Caller material copies
and two immutable payload arenas are additional, currently unbudgeted memory.
