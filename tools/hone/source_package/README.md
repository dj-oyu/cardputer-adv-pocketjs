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
