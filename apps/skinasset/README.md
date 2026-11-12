# Generated Blender synthetic input / optional Scene app

Public fixed receipt only: node 0, six vertices, four faces, two joints,
AYNM v1 cold model 624 bytes. `asset` is generated `arrays.cjs` data:
V Float32(36), J/W Uint16(24), IB Float32(24), two named samples `rest` and
`child_y6`, each G Float32(24). G/IB are mesh-local row-major 3x4 matrices.
The exporter already applied basis [x,-z,-y] and reversed winding once;
this app does neither again. Normals follow the existing skin inverse-transpose
contract; this does not claim Blender evaluated-surface-normal equivalence.

The optional `skin_asset_entry.js` uses `pocket.app.load('scene'/'sample')`.
`startGeneratedSkinSample(coldArrayBuffer, asset)` returns:
- `frame(timeMs)` evaluates and submits a front view.
- `evaluate(timeMs, back=false)` and `render()` expose synchronous snapshot phases.
- `stats(out)` fills existing Scene stats; `stop()` releases owned Scene and Model.

Times must be finite and nonnegative. The selector is a **step**, not interpolation:
rest below 1000 ms, child_y6 thereafter. Repeated time pauses, smaller time seeks.
No invented rotations, additional node transforms, Motion extrapolation or rig format.
Inputs are borrowed, not cloned: keep valid through each evaluation. Mutation after
evaluation does not mutate the native snapshot; mutation before another evaluation
is rejected by native validation when invalid. Stop is idempotent; stopped calls
throw. A pending renderer snapshot survives input mutation and stop.

`optional_chunks.txt` is deliberately not auto-discovered by default CMake builds.
Diagnostic integration must explicitly include it and select registry identity
`local.skinasset`; defaults/HOME/registry are unchanged here.

```
python3 tools/ayatsuri/skin/run_generated_app.py --output-dir .cache/generated-app-run
```

This fresh-generates Blender receipt/model/typed arrays and the independent raw
geometry oracle, then executes the shipped optional JS entry on real QuickJS/native
Scene/renderer using a host chunk-dispatch shim. The production C loader and QEMU
remain a separate acceptance. Three runtime lifetimes per strip/direct condition
cover startup-failure recovery, two poses/front/back, forward/reverse/pause, warm
native evaluation counts, IO retry, input poisoning, pending stop and final zero
requested allocations. Oblique camera [200,0,400] distinguishes the Z displacement;
shared-renderer reference is not an independent shading oracle. ASan/UBSan enabled,
LSan disabled for ptrace. Host memory is not target RAM/FPS.

Diagnostic bootstrap after generation:

```
python3 apps/skinasset/prepare.py .cache/generated-app-run .cache/skinasset-optional
```

This accepts only the fixed public receipt/model/arrays SHA256 values, emits
`skin_asset_bootstrap.js` and the optional chunk table, and exposes
`skinAssetFrame(ms)` / `skinAssetStop()`. Registry identity is `local.skinasset`.
Production C loader and QEMU stdio verification belong to the firmware lane.
