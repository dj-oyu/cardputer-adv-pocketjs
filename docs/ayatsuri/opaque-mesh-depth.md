> PR117 integration: see [current frame integration](pr117-native-mesh.md).

# Main-based opaque mesh depth candidate

This local candidate starts from the exact public main `885fb18` tree
`2bb7e1ccafede683564e8431846e123c77407416`. It extracts the required code from
PR110 and the separate depth candidate; it does not merge PR110's complete tree.
The earlier local candidate `951bdd8` is retained separately.

## Scope

- Managed Model/Instance, immutable numeric cold data, targets, validated current
  mesh/depth view, transform/shade/visibility kernels and JS bindings.
- Existing Hone Gouraud 7/8-argument painter API and its native shader/packet
  validation, plus opaque shared-depth opt-in via
  `procedural.beginMeshFrame(color, bytes, work, rows[, surface])`.
- Owned bounded candidate/committed compact snapshots, rollback, retry/ACK,
  old-committed backdrop repair, reset and depth-only full-surface damage.
- Class-owner registry dependency: main's fixed 16 process-lifetime owner slots
  cannot safely host the added lazy classes. The linked owner/reset implementation
  and its existing API-surface type adaptations are included with the regression.

No modelBuiltin/static asset loader, character demo, image, authored model,
AYNM data file or asset byte initializer is added. The pure compiler accepts
caller-provided numeric designs and has no Cloud exporter/demo dependency.
Follow/Drive adapters are outside this extraction. Model instance targets and
JS-facing shader controls remain available. No workflow file is changed.
The triangle/quad fixtures are small synthetic test source; generated binary
fixtures stay in `.cache` and are not committed.

## Depth contract and accounting

All packets of one mesh frame/surface share depth; other surfaces are separate.
Larger view depth wins. Coverage/weights use double arithmetic; interpolated
binary32 depth compares with a first-submitted-face policy for exact ties.
Near-tie correctness beyond this policy is not established. Opaque mesh scopes
reject holes, clipping and painter/analytic packets instead of dropping them.

`bytes` is an explicit per-candidate budget (1..65536); `work` sums clipped
triangle bounding-box pixels (1..8000000), plus packet/face bounds. It is not a
cycle budget or an expansion of the legacy 1024-segment serialization quota.
Rows are 1 or 8: F32 depth scratch is 960 or 7680 B. The largest requested
workspace remains allocated until reset. The native heap owns depth storage;
no source instance/JS buffer is retained after submission.

For a warmed fixed byte budget B, snapshot payload is 2*B plus scratch and
metadata/allocator overhead. Resizing prepares a replacement before freeing
old candidate storage, so both may coexist transiently. Add existing procedural
frames/workspace/VM, model cold/arena, guest heap, allocator overhead, stack and
platform baseline before admitting a device image. No PSRAM is assumed and
160 KiB guest/20 KiB stack defaults do not prove physical RAM availability.

## Checks

```
python3 tools/kasane_contract/run_mesh_depth.py
python3 tools/kasane_contract/run_pocket_proc_qjs.py --gouraud-only
python3 tools/pocket/run_class_registry.py
python3 tools/kasane_contract/run_pocket_shiki_api_qjs.py
bash tools/build_kasane_test.sh
/tmp/test-pocket-kasane
```

Synthetic tests are not real-fox/device acceptance. The former host depth
auxiliary gate's 12/36 failures at 1e-4 and near-tie uncertainty remain unresolved;
no threshold was relaxed. Formal firmware link and device RAM/timing are pending.

The formal firmware link remains pending because the available build environment
has no CMake/Ninja. Host results do not establish target link, memory capacity,
render time or device acceptance. No product default, firmware deployment or
device operation is part of this candidate.
