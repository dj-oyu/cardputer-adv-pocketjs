# Native geometry candidate

`native_geometry.py source.private.json geometry.private.h` emits immutable C
arrays from `hone.source-preservation.v1`, using the companion corner planner.
It writes only a new output and reports input/output SHA-256. Keep both the
source and generated asset private. Publish only generic tools/tests.

The typed view retains corner topology, rest positions/normals, polygon
smoothing, source indices, vertex-group weight indices, and morph coordinates
with relative-key indices. Floats are explicitly rounded to IEEE754 binary32;
this is not byte-for-byte preservation of Blender's complete scene. Source
mesh/group/key names and controls remain in the authoritative source package.

This is a compile-input candidate, **not a runtime-ready model**. It does not
infer material, opacity, transforms, modifier execution, animation, normal
recomputation, ownership or LCD registration. No private broker destination or
recipe is finalized until a real consumer exists. Do not substitute the static
opaque AYNM compiler, which would silently discard unsupported semantics.

Run `python -m unittest -v` in this directory. Synthetic tests compile and run
a C11 consumer under ASan/UBSan (LSan disabled), verify layout, morph-relative
mapping and corner/weight duplication, invalid input and output-no-overwrite.
No firmware build, target memory budget or hardware performance is established.

## Relative-morph consumer candidate

`native_morph.h` reads the generated typed view, with caller-resolved values and
per-vertex masks in exact source key order. Negative/extrapolated finite values
are allowed; masks must be in [0,1]. The point result commits only on success.
No allocation, intermediate mesh copy or virtual callback is added. Cold data
must be generated/validated and immutable, and output must be disjoint scratch.
This candidate is not wired into firmware; it does not evaluate drivers, skin,
normals or material opacity. Arithmetic is binary32, not an exact Blender oracle.

Only an explicit `use_relative: true` sets the generated relative-morph flag;
the consumer rejects absolute/time-based or unknown morph modes. Values must
already account for mute/driver/slider policy, and masks must correspond to each
source key's vertex-group mask. These are not inferred from absent metadata.
The synthetic test checks 301 scalar/mask combinations against a double
reference (1e-6 tolerance), not a Blender image or deformation acceptance gate.

The consumer's finite-value and overflow rejection requires normal IEEE floating
point semantics. Do not compile it with `-ffast-math`, `-ffinite-math-only`, or
other options that assume finite inputs or remove NaN/Inf checks. Firmware
integration must enforce this for every translation unit that includes it.

## Provider partition admission plan

`python provider_partition.py source.json mapping.private.json` computes an
order-preserving offline triangle partition for the existing 32-vertex provider.
Each part has at most 64 triangles and retains exact prepared triangle IDs.
`prepared_vertices` indexes the prepared corner-split/emitted arrays, NOT original
Blender vertices or source loop IDs. Gather all
position/morph/weight/normal channels through that same mapping during packaging.
The tool does not duplicate geometry at runtime or change triangle order.
Mappings generated from private models are private; publish only synthetic tests.

The reported geometry-node count is a lower bound on total model nodes. Reserve
hierarchy and other nodes within the 256-node model cap; material boundaries may
require additional splits. A count below 256 is not runtime admission. Node IDs,
local transforms, rig controls, material/alpha and the production caller remain
unbound. Empty meshes yield no geometry part and must retain scene-node semantics
elsewhere. This plan must not silently replace a complete scene representation.

## Source-bound layout and rig-space reference

`provider_binding.binding_plan` additionally splits at material changes without
reordering triangles. Each geometry node references its source mesh transform;
partition siblings share that evaluated transform. No transform-only parent is
silently inserted per source mesh. This is an offline binding map, not AYNM data
or a production caller. Original scene parenting and rig evaluation stay required.
Generated binding maps contain private geometry relationships and remain private.

`rig_space.mesh_palette` computes the affine bone-space conjugation
`inverse(M) * A * P * inverse(B) * inverse(A) * M`, with source mesh-world `M`,
armature-world `A`, evaluated pose-global `P` and rest-global `B`. Geometry is
returned to mesh-node-local space; applying `M` afterwards is exactly once.
The reference uses Python floating point, no coordinate-axis or TRS conversion,
and is not a target performance/precision guarantee. Integration must enforce
condition/residual and target numeric error limits and supply evaluated matrices
for each pose rather than freeze the recovered pose as an animation substitute.

This matrix composition is NOT a skinning algorithm. A source modifier with
preserve-volume requires its dual-quaternion semantics; the existing linear-blend
skin evaluator cannot silently replace it. Shrinkwrap, constraints, morph drivers,
normal recomputation and material/alpha acceptance are separate unresolved gates.
A material-bound geometry count below 256 still does not prove the complete scene
or memory budget fits. Neither helper changes `runtime_ready` to true.

For the existing rigid-DQ candidate, use `dq_factors`: transform the point into
armature space, blend `P * inverse(B)` with the existing validated DQ routine,
then transform back to mesh-local space. Do not quaternion-convert the conjugated
mesh palette: mesh scale/shear can make it non-rigid even when bones are rigid.
This preserves the existing five-pose DQ oracle's coordinate contract without
reimplementing its deformation algorithm.

## Thin DQ vertex bridge

`native_dq.h` calls the existing `source_package/rigid_dq_candidate.c` directly.
It gathers up to four weights through the emitted prepared-corner index and an
explicit group-to-joint mapping, transforms the supplied morphed position into
rig space and back, and commits one float32 point on success. More than four
influences reject rather than truncate. Palette quaternions must already satisfy
the existing proper-rigid gate, with matching same-frame transforms. Cold spans
are trusted complete generated arrays; this is not an untrusted binary parser.

No mesh copy, allocation or second DQ algorithm is introduced. Normal evaluation,
frame-level atomicity, alpha packet construction, drivers and firmware registration
remain caller responsibilities, not claims of this bridge. Link the public DQ C
source when testing. In a staging checkout set `HONE_DQ_SOURCE` to its file path;
the repository layout uses `../source_package/rigid_dq_candidate.c` by default.

The mapping must resolve every referenced group to a valid bone palette entry.
Unmapped non-bone groups reject; the four-entry bound includes zero-weight entries.
No groups are filtered and no influences are truncated implicitly. Supporting a
broader source requires an explicit, source-faithful binding contract first.
Compile the separately linked `rigid_dq_candidate.c` with normal IEEE semantics
as well: the bridge's preprocessor guard cannot inspect another translation unit.
Neither file may use fast-math/finite-math-only assumptions.

## Deform once, reconstruct full-source normals, then gather

Generated descriptors retain original source positions, polygon offsets/vertices,
source corner IDs and polygon smoothing. `source_representative` maps each source
vertex to a prepared corner carrying the same weights/morph coordinates. Evaluate
that representative once per source vertex, not once per duplicated corner.
UINT32_MAX means an isolated source vertex without a prepared representative;
it must never be dereferenced. A polygon referencing such a vertex rejects.

Compute normals on the full original polygon mesh before gathering into 32-vertex
parts. Recomputing normals independently per part would break smoothing at part
boundaries. These fields preserve the necessary topology; they do not admit the
host normal candidate or claim its remaining Blender tolerances are satisfied.

`polygon_corner_count` counts packed polygon entries, whereas `source_corner_count`
is the original normal/loop-index span. Raw `polygon_corners` may be sparse and
must only index buffers sized by `source_corner_count`, never the packed count.
Both spans are bounded. Polygons with fewer than three vertices reject; polygons
larger than quads remain source data and are not implicitly admitted by the
triangle/quad-only normal candidate.
