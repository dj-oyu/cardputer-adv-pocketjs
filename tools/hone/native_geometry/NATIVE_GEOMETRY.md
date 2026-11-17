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

`native_deform.h` composes the existing morph and DQ point candidates into a
source-vertex pass using that representative. The caller supplies resolved
same-frame values and vertex-major key masks, chooses skin only after modifier
admission, and owns disjoint unpublished output scratch. Failure may leave
scratch partially written. Isolated vertices are copied unchanged and must not
be used in polygons or modifier dependencies. Drivers, shrinkwrap, modifier
ordering and frame publication remain outside this candidate.

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

`native_gather.h` is a separate candidate for gathering up to 32 prepared corners
from one deformed source mesh and its full-mesh packed polygon normals. The
generated `corner_polygon` maps prepared corners to faces; `source_polygon`
continues to map triangles to faces. Sparse original loop IDs are resolved within
their polygon, not used as packed normal offsets. Invalid indices or non-unit
normals leave the entire output unchanged. Inputs must be disjoint and remain
alive for the call. Generated descriptors and spans are trusted complete and
immutable; this is not a binary parser. Compile without fast/finite-only math.
Validation builds a local 32-entry source/normal index plan (256 bytes), then
copies fields only after the whole plan succeeds. No vertex staging array is
needed. The input data must remain immutable between validation and copying;
the plan is internal scratch, never retained or reused across calls/frames.
This does not perform deformation, admit the normal candidate,
or register the provider; real-model equivalence and target costs remain open.

`native_provider.h` connects an already prepared frame and explicit node-to-part
map to the existing `ayatsuri_model_geometry_provider` signature. It checks node,
mesh and vertex counts and supplies the slot only after a whole gather succeeds.
The map is indexed by model node ID, including non-mesh IDs; the compact geometry
list produced by `provider_binding.py` cannot be used directly as that map.
It does not allocate, deform or commit a frame. The synthetic pipeline test calls
the real provider type, but does not call the complete model evaluator or display
path. The provider shares the validation pass and copies directly into the
correctly typed slot, without casting between vertex structs. Its private index
plan replaces both former 768-byte vertex staging arrays. A late invalid vertex,
nonfinite position/normal or non-unit normal leaves the entire slot unchanged.

`provider_node_map.py` emits private provider bindings from the partition plan
and an explicit array indexed by actual model node ID. Null entries reserve
non-geometry nodes; every geometry partition must be assigned exactly once.
The total array is limited to 256 model nodes, including holes. The caller must
derive and verify the actual scene node IDs and transforms: this tool does not
construct the scene or claim runtime admission.

`scene_preflight.py` reports geometry partitions and remaining slots before
non-geometry nodes, along with source topology, weights and modifier counts.
Missing normal or modifier inventory rejects. Its report is not a full scene
budget or a modifier, material, animation or runtime admission decision.
It also checks whether the rest triangles have nonzero area after binary32
rounding, as required by ordinary cold AYNM admission. Never remove collapsed
triangles merely to pass this check: later morphs may unfold them.

`scene_hierarchy_inventory.py` snapshots evaluated Blender object transforms
and verifies source hashes, supported parent types, finite controls and absence
of active object constraints. `scene_node_plan.py` assigns parent-first model
nodes and explicit provider IDs; it does not evaluate later animation or infer
camera, lights, material or rig semantics. `driver_route_plan.py` inventories
every driver output and retains unbound routes explicitly. The candidate
`driver_node_targets_candidate.h` stages admitted direct-XYZ parent pose and
mesh-visibility outputs into caller-owned model target arrays. Input freeze,
remaining rig/morph/shader outputs and whole-frame publication remain external.
`emit_driver_morph_targets.py` separately routes admitted relative, unmasked,
unmuted shape-key values into disjoint caller-owned storage. Masked, absolute,
missing or muted source keys reject; it does not evaluate rig or modifiers.

`scene_cold_probe.py`, `compile_provider_probe.cjs` and
`run_scene_provider_probe.py` exercise a provider-only cold topology with explicit
placeholder materials. They are host diagnostics, not firmware-ready assets or
rendering correctness evidence. The probe must never be flashed or presented as
the finished model. The provider-only open path rejects ordinary cold fallback;
all active meshes must be supplied by the native provider every evaluation.

`frame_memory_plan.py` counts explicitly sized candidate arrays, and
`native_normals_batch.h` shares the normal workspace across sequential meshes.
The result excludes model/snapshot arenas, rig, alpha, display, ABI padding and
stack. Pending geometry remains owned and live for the whole provider pass;
these estimates are not target memory or performance measurements.

`material_route_plan.py` checks polygon/slot ownership, graph links and
shader-driver socket identities. It retains unsupported graph features as
explicit pending work; it does not evaluate colors, opacity or lighting.
`alpha_factor_compile.py` additionally admits a restricted white-transparent /
opaque-principled mix and compiles the scalar factor with object-local XYZ and
driver values. Division by zero, invalid roots and nonfinite inputs reject.
The scalar candidate is not a full shader: source color, specular/lighting,
clipping and perspective-correct local-coordinate interpolation remain open.
Its nonlinear factor must be evaluated at the appropriate fragment coordinate;
interpolating per-vertex alpha would not preserve the graph.
`base_color_compile.py` separately admits a bounded scalar/color graph with
MIX and linear RGB ramps and produces candidate linear RGB input values.
It uses three independent scalar programs and caller-owned scratch. This is
not Principled specular/lighting, color-management, clipped attribute
interpolation or alpha-compositing admission, and no target cost is established.
`alpha_graph_fragment_candidate.h` joins those programs to orthographic
barycentric interpolation of a preclipped triangle's deformed object-local XYZ.
It returns an unlit fragment, not a shaded/rasterized frame; callers must
preserve local attributes through clipping and freeze driver values per frame.
`alpha_graph_raster_candidate.h` composes preclipped graph fragments against a
caller-supplied full opaque-depth image, with whole-frame preflight and bounded
four-fragment sorting. Its RGB remains unlit, and it is a host-only diagnostic:
source clipping, shared row-depth ownership, lighting, DMA, publication and
target memory/time have not been integrated.

`shrinkwrap_route_plan.py` records nearest-surface dependencies and rejects
cycles and unsupported modes. `native_wrap_candidate.c` transforms current
source positions into the current target's coordinates, requires a rigid
relative transform, applies the admitted ON_SURFACE offset and converts back.
Target deformation, bone evaluation, modifier ordering, workspace ownership
and normals remain caller responsibilities. Candidate output is unpublished and
may be partially written on failure; no target timing is established.

`rig_root_inventory.py`, `emit_root_rig_targets.py` and
`rig_root_palette_candidate.h` admit a restricted rig with root-only,
constraint-free, unscaled deform bones and explicit group mapping. Driver
rotation outputs feed a staged quaternion/translation palette. Mesh/rig space
transforms in the inventory are snapshots and must be recomputed or proved
constant for the active frame. Parented bones, arbitrary constraints and scale
do not silently fall back to this candidate.

Host GCC/x86-64 `-fstack-usage` comparison with the same external callback wrapper:
before, O2 provider 1,664 bytes plus wrapper 8; after, the inlined wrapper is
384 bytes. O0 provider + validation/gather frames changed from 864 + 896 to
352 + 128 bytes (the wrapper remains 48). These are host compiler frame sizes,
not ESP32 stack high-water or timing results; caller-owned slot storage and model
evaluation frames are additional. Target stack headroom is not established.
The pipeline test runs O0/O2 with strict aliasing, checks full 32-entry mappings,
late failures and retry, and retains the full-mesh-normal-before-gather sequence.
