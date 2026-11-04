> PR117 integration retains these historical measurements; current entry and
> validation commands are in [pr117-native-mesh.md](pr117-native-mesh.md).

# Explicit material sidedness (local candidate)

This candidate is based on the published a426f7a tree (equal-tree local f7d7fe6c).
It changes no Blender original, camera/light/pose/scoring condition, renderer
quota, color space, light approximation or runtime default. No public upload,
PR, device test or firmware operation is part of this work.

## Import, cold representation and compatibility

`import_glb.cjs` preserves glTF `doubleSided`. All-single imports retain the
legacy IR v1 output; double-sided imports use `hone-scene-ir-v2` with explicit
`materials[].sidedness` and a single-sided default material. Unsupported alpha,
textures, skin and morph still reject. An exporter that already erased the flag
cannot be repaired by import: the comparison environment must stop overriding
all exported materials to single-sided while retaining its original baseline.

`compile_static.cjs` is an exact static import/batch/compile adapter, not a QEM
or character generator. It requires explicit immutable scope mappings and shader
material mappings. Ordered batching only merges adjacent compatible surfaces
and partitions the original face sequence without sorting noncontiguous faces.
32 vertices/64 faces per part remain enforced. Material dedup includes sidedness.

Design v2 requires `materialSidedness` on meshes; v1 cannot silently carry that
field. All-single designs, including explicit single v2, emit the old bytes.
When double-sided is present, required AYNM section 13 has stride4/count=material
count: U8 0=single or1=double, then three mandatory zero bytes. Its directory flag
is1 (required), so old loaders reject. Previous formats assign only sections
1..12; descriptor bytes19..31 stay mandatory zero. Unknown required sections,
wrong counts/flags/values, padding and truncated inputs reject transactionally.

The C model section array grows by one entry (8 bytes); rebuild consumers with
the current header. This is cold-data compatibility, not C binary ABI compatibility.

## Evaluation and native adapter

Only double-sided nodes use packet kind5 and
`ayatsuri_model_instance_sided_mesh_view()`. Old `mesh_view()` refuses them.
Kind4/legacy paths are unchanged. Kind5 exposes a stable borrowed positive-winding
patch, shared F32 depths, back color pairs and one back-facing bit per triangle.
The extra aligned instance cache is `align8(4*vertices+8)`, at most136 B per
sided mesh; cold geometry/normals are not duplicated.

Back normals are inverted in transformed full precision before SNORM rounding,
then sampled through the unchanged shader for A and B. Mask/albedos are not
exchanged. Logical face side uses the runtime determinant once; raster winding
normalization is separate. Static import already bakes/corrects its determinant,
and the static adapter emits identity poses instead of reapplying that matrix.
Legacy single reflection behavior remains unchanged.

For kind5 in a shared-depth scope, call `ksn_mesh_append_sided(snapshot,
view.patch, view.depth, view.back_colors, view.back_faces, node_id)` exactly once.
Both managed submit methods use that path. A standalone native adapter must
switch from the d3fdb778 per-run append loop to this call. Model cursor and
stats.packets count logical draw nodes; each nonempty depth mesh now emits one
record. The old `ksn_mesh_sided_run()` remains only for the painter fallback and
the regression reference. No face is reordered or removed.

The explicitly tagged compact mode2 stores front/back A/B per vertex and one
side bit per face. It does not reinterpret reserved fields. Older readers reject
mode2; old modes0/1 are unchanged. Equal palettes, all-front, or all-back meshes
losslessly use the old encoding with the selected palette. Mode2 costs
`align4(12 + 24*referenced_vertices + 3*faces + ceil(faces/8))`: at most980 B for
32 vertices/64 faces, versus844 B in the old general mode. Source data is copied,
not retained. Unknown modes, unused side bits, padding, invalid indices,
nonfinite geometry, truncation and over-budget appends reject before mutation.
The renderer selects colors once per triangle, outside the pixel loop; coverage,
mask arithmetic, depth interpolation and first-face equal-depth ties stay intact.

Both submit methods retain existing whole-call mark/rollback. Candidate and
committed snapshots own numeric data through disposal, retry and ACK. Depth
scratch, 256-record, byte and work quotas are unchanged. Holes/analytic/painter
inputs remain unsupported in the depth scope.

## Historical 4a4d reproduction notes

```
python3 tools/kasane_contract/run_mesh_depth.py --opt 2
python3 tools/pocket/run_class_registry.py
```

The PR117 integration uses the current Shiki frame API and no removed float VM.
Historical paired a426 captures remain in the private 4a4d handoff.
The runner checks baseline blob identities before compiling. It downloads
nothing and uses the same VM, renderer, flags and inputs in paired legacy
captures. Optional `--legacy-input PATH` adds an existing old single asset;
inputs and rendered binary captures remain local/.cache and are not committed.

Synthetic O0/O2 ASan/UBSan checks pass. Legacy compiler comparison covers100
single designs plus explicit-single v2 equivalents. Paired old/current actual-C
renders matched all bytes for6 inputs x4 conditions (three generated controls
plus the three existing public cloud/fox-face/fox-sheet assets). Those optional
old assets are not copied into this candidate. The earlier 660-byte mesh fixture
retains SHA256 59d431624fb828018a12e8540853f04d6f29366110269b59c649db7dc9fe3710.

Additional checks cover material policy separation/immutability, positive
winding, independently calculated inverse-scale back lighting, runtime mirror,
static determinant correction, centroid ordering, run output alias rejection,
32-vertex cache bound, both submit methods, post-disposal drawing, first-face
depth ties versus last-face painter order, and whole-call byte/record rollback.

## Record amplification correction and measured costs

The previous d3fdb778 design copied each contiguous side run to a separate
record. The private fixed-input accounting probe found front564/oblique543
records for94 nonempty logical packets and1166 triangles, beyond256. Its
rich used bytes60088/59748 (sweep maximum60112) remained below65536; work56462/
54203 below8000000. These are accounting-only results, not successful rendering.
The original, partition, face order, admission and scoring are not changed here.

Alternatives considered: raising quotas leaves amplification unresolved;
reordering nonadjacent faces changes ties; doubled vertex palettes violate the
32-vertex boundary; a shared geometry table adds cross-record ownership and
validation. A self-contained face-side record is the smallest local change.
It keeps one record per logical mesh without a new lifetime or geometry table.

Synthetic64 alternating faces sharing3 vertices compare the old ordered runs
against one side-aware record. Both first-front and first-back cases retain
RGB and F32 depth byte equality, including A/B mask boundaries and exact-depth
ties. With varying A/B colors, records64->1, occupied bytes4864->284; equal
unlit palettes3328->240. These differences do not remove triangles. The managed
single control selects32 faces (144 B); sided64 selects64 faces (284 B), with
unchanged work14112/28224 respectively. Five logical nodes now submit as5
records. A252-record prefix followed by five nodes still rejects and rolls back
the whole call; separate byte/work partial-failure tests do the same for both
submit variants. Destroying source model/instance before render retains pixels.

Representative O2 ASan/UBSan CPU samples (8 repeats, host only):

| Test | Ordered run reference | One side-aware record |
|---|---:|---:|
| Varying A/B, first front, raster | 6.726 ms | 3.954 ms |
| Varying A/B, first back, raster | 6.549 ms | 3.974 ms |
| Unlit, raster | 5.278 ms | 3.181 ms |

The integrated warm submit+render is4.449 ms (`submit`) /4.341 ms
(`submitClipped`) for sided64; the single32-face control is2.208/2.004 ms.
These are current samples, not a cross-run historical speedup claim.

No new model arena/cold storage is introduced by this correction. The original
sidedness cache remains at most136 B per32-vertex mesh; synthetic arena1644->1780 B.
The private probe's cold40292 B and arena122620 B (old118300) therefore remain
unaffected. Mode2 worst-case is980 B/record (+136 B over old general encoding).
94*980=92120 B exceeds65536, so reducing record count alone does not prove byte
admission. Actual private front/oblique used bytes must be measured again.

Candidate+committed allocation capacities do not shrink merely because occupied
bytes shrink: the host stress suite requests65536 B each,131072 B total, plus
960 B depth scratch, metadata, allocator, old frames, VM, guest/model and stack.
The append adapter uses a640-byte vertex scratch; all-back palette collapse has
a separate noinline640-byte scratch. The1040-byte painter-run patch is isolated
from the depth branch. These are source-level sizes, not measured peak stack.
Raster CPU and managed warm-submit/render measurements are logged by the
commands above; ASan/UBSan host times are not firmware performance. Formal
firmware build, actual total RAM and device testing were not performed here.

The real fox is private in another environment. Its7/5-pixel sidedness failure,
quality FAIL, earlier depth auxiliary gate and near-tie uncertainty remain open.
No Blender reproduction percentage or60% attainment is claimed. Next is the
fixed v2 native adapter integration and independent shape/shading/light-response
re-evaluation, without changing originals, partitions, conditions or budgets.
