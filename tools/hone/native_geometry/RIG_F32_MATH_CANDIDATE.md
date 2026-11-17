# Independent ordered rig math candidate

`rig_f32_math_candidate.h` is **opt-in host math**, not a full armature evaluator,
provider integration, runtime-ready model, or target-performance result. Existing
rig, palette, wrap and publication paths are unchanged. No private asset or
extracted model coefficient is present in these sources or synthetic fixtures.
The formulas were independently derived and tested through Blender black-box
outputs; no GPL implementation was transcribed.

## APIs and explicit admission

- `hone_rig_f32_euler_xyz`: finite binary32 XYZ angles -> row-major 3x3 rotation.
  Trigonometry and matrix expressions use double, then each matrix element rounds
  to float. This matters: using `sinf/cosf` and float expression intermediates
  produced different outputs. Caller-side RNA-style angle conversion is not
  inferred from a double driver result.
- `hone_rig_f32_matrix_quat_positive_trace`: proper-rigid 3x3 -> wxyz, after float
  column normalization. The admitted domain requires positive normalized trace
  **and nonnegative diagonal entries**. This is a conservative identified branch,
  not every positive-trace rotation. Other rotations, scale, shear, reflection,
  non-finite and singular matrices fail without writing output. The quaternion
  result is not forcibly re-normalized again; that changes observed rounding.
- `hone_dq_f32_position`: point plus proper-rigid wxyz/translation palette in the
  same space; up to four nonnegative finite influences. It selects quaternion
  hemisphere from the first nonzero influence and uses the standard homogeneous
  DQ expression with reciprocal real-norm-squared at the end. It does not first
  construct a unit blended quaternion or orthogonalize the blended dual part.
  Positive-weight input quaternion squared norms must be within `1e-6` of one.
  No influence copies the point. Zero/underflowed blend norm, overflow and invalid
  indices fail. Zero-weight indices and weights are validated; unused joint
  values are not inspected. Counts above 65,536 joints are rejected.

All complete input spans remain immutable during a call. Output commits only on
success, including late arithmetic failure. Float input/output overlap is safe;
uint32 index arrays must be disjoint from float spans, with no cross-type casts.
The caller owns synchronization, world-space coherence, palette identity and
frame lifetime. These helpers retain nothing and allocate nothing.

Use the binary32/compiler contract from `affine_f32_candidate.h`, including
`-ffp-contract=off`, no excess precision, no fast-math and no reassociation.
The observed rounding is not a promise about other Blender versions, C libraries,
architectures or compiler flags. Admission tolerances above do not relax any
vertex or normal gate.

## Reproduction

```sh
python tools/hone/native_geometry/test_rig_f32_math_candidate.py
HONE_SANITIZE=1 python tools/hone/native_geometry/test_rig_f32_math_candidate.py
python tools/hone/native_geometry/test_rig_f32_math_blender_oracle.py --blender /path/to/blender
```

Windows verification used UCRT64 GCC 15.2.0 and Blender 4.3.2
(`32f5fdce0a0a`). O0/O2 contracts pass: atomic failure, float aliasing,
non-finite values, invalid indices/weights, antipodal quaternion hemisphere,
zero influences, blend underflow/overflow, invalid quaternion norms, matrix
scale/reflection and unsupported branch rejection. Independent Linux ASan/UBSan
runs pass the same test. Each optimization also compares 4,000 random DQ cases
with the existing independent double consumer, maximum component difference
`9.24313399e-7`. That numerical-envelope test is not a full-frame admission gate.

The public Blender test uses only newly generated synthetic objects and saves no
blend file. Results:

| Check | Result |
| --- | --- |
| Euler matrix comparison | 36,000 components exactly equal |
| Admitted matrix/quaternion comparison | 9,416 components exactly equal |
| Unsupported rotation-domain samples | 1,646 rejected with output unchanged |
| Synthetic two-root Armature DQ | 4,096 vertices; maximum component error `2.384185791e-7` |

The Armature test supplies evaluated pose matrices to isolate DQ arithmetic; it
is not proof of native driver, parent, rest-palette or space-transform generation.
Component equality does not promise identical signed-zero bits.

## Private staged diagnosis and remaining gates

A private root-palette fixture switched to these exact public helpers and
retained the previously identified ordered affine transforms. For the two
post-wrap meshes in four simultaneous control samples, native rigged-target
positions plus the diagnostic wrap/normal consumer produced normal component
maxima of approximately:

`4.17e-7 / 7.15e-5 / 6.47e-5 / 2.98e-7`

No compared corner exceeded the existing `1e-4` normal gate. Post-wrap position
maxima were `0 / 2.3842e-7 / 1.1921e-5 / 0`. The third sample therefore still has
a near-surface offset position discrepancy. These are staged host measurements,
not a whole-scene native success: source morph positions and world maps still
include oracle inputs, and the default provider does not select these helpers.

The offset discrepancy reproduces on a synthetic tilted plane at very small
surface distances. Query and zero-offset nearest positions agree; the nonzero
offset response differs. Recomputing the selected-face offset direction with
double closest geometry reduced position discrepancy but worsened normal gates,
so that experiment was rejected. No guessed normal substitution, coordinate
snap, data-specific correction or widened gate was adopted.

Next: independently identify near-surface offset behavior; then supply native
source morph and native world maps to the same-frame whole-scene provider test,
including normals, materials, rollback and source-buffer lifetimes. Only after
that evidence passes can a same-revision private firmware build and physical
DMA/FPS/recovery measurement establish runtime readiness. Existing-demo FPS and
a model-free firmware build do not satisfy this requirement.

No target cycle/stack/heap saving is established. The homogeneous expression
changes normalization/division work, but benefit cannot be inferred from host
correctness or unrelated ESP32 kernel measurements. Target-model DMA/FPS remains
mandatory and unmeasured by this work.
