# Public synthetic DQ oracle (test-only)

This rebuilt candidate replaces a lost volatile experiment; it is not byte-identical to the previous scripts. Only public authored constants are used. No character model, private rig, product implementation, or AYNM/wire change is included.

From the repository root:

```
blender --background --factory-startup --python tools/ayatsuri/dq-oracle/blender_dq_fixture.py -- /tmp/public-dq-fresh
python3 tools/ayatsuri/dq-oracle/verify_dq_fixture.py /tmp/public-dq-fresh/receipt.json
```

Use a new output directory. Python assertions must remain enabled (do not use `-O`). The generator saves a synthetic .blend and receipt; generated assets are not committed.

Fixture: 6 grid vertices, 4 triangles, 2 identity-oriented bones; child rotates 60 degrees about normalized (1,0,1), at pivot (1,0,0). Child weights are 0, 0.5, 1. Identity objects, no scale/shear/morphs/partitions. Blender evaluates both Preserve Volume DQ and LBS.

The independent oracle builds rigid dual quaternions and compares their normalized hemisphere-aligned blend against a separate known-axis/common-pivot closed form. It compares Blender positions and flat triangle face normals derived from independently predicted deformed vertices. Transported fixed-weight DQ normals are output separately; they are not the surface normals of spatially varying weights. The sign of real and dual components must change together.

Fresh rebuilt run: Blender position maximum error 9.692397440197453e-08; maximum DQ/LBS distance 0.1640846996117676 in this fixture's units. Ten rejection checks cover zero/negative/nonfinite weights, invalid quaternion norm/Study condition including zero-influence transforms, LBS substitution, real-only antipodal sign flip and missing normalization. Antipodal invariance and zero-weight endpoints are positive tests. This tolerance is a fixture numeric check, not a user quality threshold.

Receipt SHA256: 6e9137f9794e166832d6ea1a103a17360ff8f01ee893161b564494a6ce4c7d04. Fresh results include the scripts' hashes; regenerate after any source change.

Not accepted: arbitrary or wide-angle blends, non-rigid transforms, skin runtime, public importer support, shader/normal lighting, character fidelity, device memory or FPS. In particular, 0.5 is not exactly representable as Uint16/65535: target quantization needs its own expectations. Existing LBS behavior must not be silently replaced. A future native DQ experiment must explicitly declare the method and normal semantics and match the raw positions and normals independently, before any render/performance claim.
