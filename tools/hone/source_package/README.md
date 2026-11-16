# Source-preservation intermediate v1

This is a lossless JSON serialization of selected mesh geometry fields, not a full Blender scene interchange or runtime-admitted asset. `runtime_ready` MUST remain false until the separate compatibility gates pass. Source SHA and Blender version bind every package to its input.

Each mesh preserves polygon smoothing and material indices, triangulation-to-polygon and loop mappings, source corner normals, sharp-edge indices, UV layers, named vertex-group weights, object/world/parent transforms and modifier declarations. Shape-key blocks preserve absolute coordinate arrays, relative-key names, values, interpolation and masks. Armature rest matrices and current pose matrices are retained. No original source is rewritten; explicit object exclusions apply only to output membership.

Do NOT replace this contract with global smooth normals. Flat faces need independent corner normals, smooth groups need matching adjacency, and normals must be recomputed/evaluated for each deformation. Current corner normals represent the source/rest mesh only, not a proof of animated deformation equivalence.

Not yet preserved: material node graphs and opacity evaluation, full modifier properties, animation actions/drivers/constraint definitions. These are explicit unresolved gates, not silently accepted defaults. Retain the original .blend as authoritative input. The JSON alone cannot reproduce the whole scene.

Synthetic validation: mixed smooth/flat two-face fixture, morph coordinate roundtrip, reject missing morph vertex and invalid corner index. This contract does not imply same-head firmware build, runtime image equivalence or real-device FPS results.

Private outputs (`*.private.json`, real input names/content, logs binding the source) must not be published. Only generic exporter, synthetic test and generalized contract may be considered for public code checkpoints after normal review.


## Commands (Blender 4.3)

```sh
blender -b --factory-startup --disable-autoexec -t 1 --python tools/hone/source_package/test_synthetic.py
blender -b --factory-startup --disable-autoexec -t 1 --python tools/hone/source_package/export_package.py -- input.blend output.private.json
```

Optional arguments after the output path are exact object names to exclude. Unknown exclusions fail. Keep generated output outside public repository staging.

## Consumer compatibility checkpoint

`consumer_plan.py` validates mesh/morph/corner references with explicit errors and plans a corner-split transient geometry mapping. It preserves source vertex/corner and polygon IDs so expression positions and skin weights can be duplicated without welding away a hard edge. Stored corner normals are rest/source data, not deformed normals. Run `python tools/hone/source_package/test_consumer_plan.py` for pure-Python synthetic tests.

This is a host adapter plan, not an AYNM loader. `admit_runtime` always rejects. Existing `ayatsuri_model_instance_evaluate_meshes` requires matching cold vertex counts and node-local unit normals; newly split topology cannot be submitted against the old cold mesh. Cold topology generation, deformed corner normals, skin/transform mapping and material opacity remain mandatory work. No production rendering path changes here.

`cold_geometry.py` partitions the corner plan into material-homogeneous candidates within the existing AYNM 32-vertex/64-triangle mesh limits. Source triangle and vertex mappings bind cold and transient geometry to one topology hash. This is not binary AYNM generation: hierarchy/node allocation, material encoding and unit-normal/coordinate admission remain unimplemented. Count hierarchy nodes against the model's 256-node cap before any future admission.

The experimental deformed-corner evaluator is angle-weighted for all-smooth triangle neighborhoods and retains flat face normals separately. It rejects non-triangle polygons, sharp edges and custom normals rather than guessing their semantics. Only synthetic deformation checks currently exercise it; Blender equivalence and production use are not established. Run `python tools/hone/source_package/test_cold_geometry.py`.

`test_quad_deformation.py` is a Blender synthetic regression for a rest-collapsed quad that opens under a morph. Rest-degenerate faces must not be deleted merely to satisfy a static importer: they may become visible later. It checks loop/vertex correspondence, not normal equivalence. Nonplanar polygon normals cannot be replaced by independently computed triangle normals without an oracle comparison. Runtime handling of zero-area corners remains unresolved.

`blender_corner_oracle.py` supplies the Blender 4.3 host reference for polygon/corner normals at supplied deformed positions, retaining rest-collapsed triangles. It rejects a changed triangulation, sharp/custom-normal inputs and non-unit oracle results. `test_blender_corner_oracle.py` checks a synthetic collapsed/open quad and Blender's +Z fallback. It is a Blender-dependent reference, not an independent native algorithm or runtime implementation; reproducing values through the same API proves mapping/reproducibility only. Source modifiers, skin and animation must already be evaluated into the supplied positions; this helper does not evaluate those systems.

`native_polygon_normals.c` is an independent experimental host candidate using binary32 Newell face contributions in last-to-first cyclic order and corner-angle-weighted smooth normals, with +Z fallback for zero contributions. It supports 3/4-sided polygons only, no sharp/custom-normal semantics. It is NOT admitted to the device path: finite-sample agreement does not establish whole-model Blender equivalence. Synthetic unit/degenerate/rejection tests do not close that equivalence gate. Build `test_native_polygon_normals.c` with `cc -std=c11 -O2 -ffp-contract=off ... -lm`; ASan/UBSan may be enabled for host tests.


Near-collapse analysis distinguishes exact zero fallback from nonzero, cancellation-sensitive polygons. The Blender 4.3.2 cached face path used by corner normals uses Newell accumulation; its per-face inspection helper can use a quad diagonal instead. See [upstream implementation](https://github.com/blender/blender/blob/v4.3.2/source/blender/blenkernel/intern/mesh_normals.cc). The candidate aligns binary32 polygon accumulation order while retaining an independent angle-weight implementation. `test_native_blender_oracle.py` compares six entirely synthetic near-collapse samples (component tolerance 1e-4). This tolerance is a host test, not runtime admission or a hardware performance target.

```sh
cc -std=c11 -O2 -fPIC -shared -ffp-contract=off tools/hone/source_package/native_polygon_normals.c -lm -o /tmp/polygon.so
blender -b --factory-startup --disable-autoexec -t 1 --python tools/hone/source_package/test_native_blender_oracle.py -- /tmp/polygon.so
```

Cost remains unmeasured on target. The prototype uses one polygon normalization, two edge normalizations and an acos per smooth corner, plus vertex normalization; work storage is 24 bytes per source vertex. Do not extrapolate host equivalence to device frame rate.

Two additional Blender synthetic contract tests remain separate from device admission:
- `test_combined_rig.py`: combines two relative morphs with a single rigid-weight bone and preserve-volume modifier, comparing evaluated positions to an independent matrix calculation over four poses.
- `test_opacity_frame_contract.py`: updates bilateral/side driver inputs and render-object transforms together, checks inverse-world recovery of object-local coordinates, and retains fractional scalar factors rather than converting them to booleans. This is not a GPU shader/alpha renderer comparison.

Neither fixture validates a private multi-bone rig, combined production driver graph, procedural material rendering or target performance. Normals computed from Blender-provided deformed positions do not independently validate native skinning.

`source_snapshot.py` is a host-only preserved-pose verification route. It validates frame/source/topology identity, exact cold corner mapping, finite/ranged positions and unit normals, then serializes matching little-endian position/normal rows without re-synthesizing authored normals. `test_source_snapshot.py` covers split flat normals and identity/unit-normal rejection. It does not implement continuous animation, native rig evaluation, shader opacity or device admission, and must not replace those development goals. Snapshot storage costs 24 bytes per split vertex per stored pose, before metadata; avoid treating a large pose bank as a free runtime solution.

### Experimental continuous angle-response LUT

`generate_wedge_lut.py` constructs analytic pairs of triangles and observes their Blender vertex-normal ratios. It does not copy upstream implementation code or approximation coefficients. `synthetic-wedge-lut.json` records the observations; the final scaled-response endpoint is linearly extrapolated from its two neighbors to avoid a numerically degenerate training wedge. `native_wedge_lut_candidate.c` uses a 65-entry/260-byte table and continuous interpolation of the scaled response. This is an experimental continuous-angle path, not a saved-pose bank or an approved runtime replacement.

Hold-out `test_wedge_lut_oracle.py` uses independently seeded random off-grid angles plus boundaries. It explicitly expects rejection where Blender returns zero normals that cannot satisfy the current runtime's unit-normal requirement; it does not substitute +Z. `test_wedge_lut.c` tests monotonicity, knot continuity and endpoints. Compile with `-ffp-contract=off`. The candidate may modify host output/work buffers before rejecting: it is not a failure-atomic runtime API.

Host timings did not establish a speed advantage over exact acos. Work remains 24 bytes/source vertex plus the constant table; target CPU timing, mixed sharp-normal semantics, arbitrary pathological geometry, native rig equivalence and renderer alpha remain unverified. The synthetic LUT tests and private sample comparisons do not close those gates. No private model or pose data participates in table generation.

### Existing caller host experiment

`python tools/ayatsuri/skin/run_render.py --wedge-lut` connects the candidate to the existing skin -> Model -> snapshot raster test, without changing the production/default path. Numeric output is unpublished candidate storage; a deliberately rejected zero-normal geometry must leave the existing Model and rendered snapshot unchanged, and a subsequent valid candidate retries successfully. Existing owner/generation, invalid-batch, borrowed-input lifetime, projection recovery and quota retry checks also run. This verifies frame-level publication, not atomic writes inside the numerical function. The diagnostic test's temporary arrays are not a production memory budget. Private rig, arbitrary geometry/material admission and hardware cost remain open gates.
