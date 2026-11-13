# Precision is part of the model contract

The existing rectangle fixture intentionally compares captured JavaScript Numbers
against the binary64 literal `0.98` before building Float32 geometry. This remains
its synthetic contract; this supplement does not alter the adapter or renderer.

A different pipeline can first quantize a control to binary32 and compare with a
binary32 threshold. At `0.97999999`, `x >= 0.98` is false, while
`Math.fround(x) >= Math.fround(0.98)` is true. Run the independent numeric example:

```sh
node tools/blink_bridge/test_precision.cjs
```

Therefore passing the rectangle bridge does **not** prove the final authored
model's visibility semantics. Do not copy its binary64 threshold rule into that
product or change an existing native precision contract merely to match this fixture.
Before model integration, pin the actual source and evaluate its dependency graph;
identify the rounding stages for controls, driver evaluation, geometry and normals
independently. Precision, object mapping and material behavior belong to that
explicit source-specific contract, not a generic Scene threshold API.
