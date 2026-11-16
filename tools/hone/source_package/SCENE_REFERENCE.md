# Minimal scene comparison reference

`visibility_plan.py` evaluates boolean visibility using the bounded driver
subset before candidate publication. Duplicate owners, unsupported expressions
and non-boolean results reject. Defaults remain explicit; no silently ignored
hide-render drivers. Run `test_visibility_plan.py`.

`alpha_triangle_reference.py` is deliberately slow host-only orthographic
triangle rasterization with top-left edge ownership, per-pixel depth sorting,
continuous alpha and an interpolated object-local scalar band mask. Visibility,
opaque occlusion, alpha ordering and row/slot capacity are evaluated separately.
Run `test_alpha_triangle_reference.py`. PBR lighting, perspective, clipping
across the camera plane, GPU antialiasing, Model packets and LCD are not covered.

RGB is flat linear floating-point to isolate geometry and transparency from
lighting. This is not the production RGB565 transfer/color-space decision.
Exact opaque/alpha depth ties favor the opaque surface; transparent ties put
first-submitted face on top. These explicit conventions require comparison
against the intended scene contract, not assumed Blender equivalence.

The function returns a fully-owned candidate or raises; it does not mutate a
committed frame. It allocates whole-frame fragment lists for reference use,
so is not the low-memory implementation and has no MCU timing claim. Native
rig/driver positions and actual material graphs are not yet wired into it.
Saved pose images and passing synthetic cases do not complete animation.

Depth interval admission is now explicit: wholly outside triangles are excluded,
wholly inside triangles accepted, and crossing triangles reject until a real
near/far clipper exists. Rejection must not be mistaken for successful clipping.

The source exporter preserves SHRINKWRAP target/auxiliary target, mode/method,
offset, group and projection/culling settings in modifier-list order. It does
not bake or silently discard that modifier. Relative shape evaluation precedes
modifier processing; target evaluated geometry/transforms are a dependency of
wrapping. `test_shrinkwrap_contract.py` checks a synthetic plane with four morph
weights and preserved settings. Native curved/moving-target shrinkwrap remains
unsupported and those meshes must remain rejected by the runtime consumer.
