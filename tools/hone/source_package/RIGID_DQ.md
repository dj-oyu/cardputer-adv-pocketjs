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

## Optional prepared palette candidate

`rigid_dq_prepare_joint` validates one resolved joint and stores its rotation and
unscaled translation-quaternion product in caller-owned `rigid_dq_joint` storage.
`rigid_dq_position_prepared` shares the original blend and point-transform code,
but omits repeated joint validation and product construction per influence.
The original `rigid_dq_position` remains available with its original checks.
Weights, indices, finite input/output and cancellation checks remain per vertex.
Neither order nor precision changes; in particular the product is not scaled by
one half during preparation, which would change the multiplication order.

Only successful preparation outputs from the same immutable pose may be passed
to the prepared API. It is not an untrusted-data parser. A joint output is
unchanged on preparation failure; callers stage and validate the entire required
palette before publishing it. Unused invalid joints need not be prepared, but
every nonzero-weight referenced joint must be. Reprepare after pose changes;
pointer equality is not a generation check. No cache, allocation or firmware
caller is introduced. Double overflow in the product is retained so the vertex
result fails just as the original does, instead of changing preparation admission.

Each prepared joint holds eight doubles (64 bytes on the tested host), versus
seven (56 bytes) in the raw input. Keeping both palettes costs their sum; do not
assume raw storage can be released if another consumer still uses it. Work saved
is one seven-value finite scan, quaternion norm check and quaternion product per
nonzero influence, replaced by once-per-prepared-joint work. No target timing or
20-fps result is established. Compile all consumers without fast/finite-only math.

`test_rigid_dq_prepared.c` links `rigid_dq_candidate.c` and compares exact bytes
and return status over 10,000 deterministic palettes, 0–4 influences, invalid
weights/indices, preparation rollback, antipodal and extreme inputs. Define
`DQ_FROZEN_ORACLE` and separately link the prior implementation renamed to
`frozen_position` to check the unchanged original path independently as well.
