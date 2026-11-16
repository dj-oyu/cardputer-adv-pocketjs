# Experimental rigid deformation consumer

This host-only candidate preserves rigid dual-quaternion blending rather than
silently substituting linear skinning for preserve-volume deformation.
`rigid_dq_consumer.py` is an independent numerical reference; the C candidate
accepts resolved unit quaternions plus translations, up to four influences.
Matrices with scale, shear or reflection are rejected by the reference converter.
The C caller must supply this validated rigid representation and valid spans.
No GPL implementation or coefficients were copied.

Run `python test_rigid_dq_consumer.py` and `python test_rigid_dq_native.py`
(requires NumPy and a C compiler). Compile `test_rigid_dq_candidate.c` with
`-lm`, optionally ASan/UBSan. The single-vertex C output is unchanged on failure;
a mesh-level caller still needs unpublished candidate ownership and commit.

Relative morph evaluation uses each key's actual relative-key coordinates.
`mask_opacity` preserves a continuous max-driven transparent-to-opaque factor
and strict object-local band comparisons; it is only this scalar graph subset.
Do not convert it into a binary visibility flag. It is not a renderer alpha test.

The inputs are same-frame resolved palette/control values. Native driver and
constraint evaluation, arbitrary non-rigid skinning, production integration,
full mesh atomic publication, renderer alpha and device cost are not admitted.
The C prototype uses double arithmetic; no MCU speed claim is made. Shape-key
vertex-group masks and absolute keys are not implemented by this reference.
