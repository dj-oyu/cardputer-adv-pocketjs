# Ordered binary32 affine candidate

`affine_f32_candidate.h` is an independent host candidate. No existing provider,
wrap implementation, firmware path, publication owner, or model ABI selects it.
Passing this candidate's tests is **not** admission of a complete native frame.
No private geometry, extracted model constants, or GPL implementation is included.

## Contract

- `hone_affine_f32_relative(target, source, out)` accepts complete row-major 3x4
  binary32 world matrices and writes 24 floats: source-to-target followed by its
  inverse. Each world matrix and both resulting maps must be proper rigid.
- Finite values, column orthonormality (absolute admission tolerance `1e-5`),
  positive determinant, and finite intermediate results are checked. Scale,
  shear, reflection, singular maps and overflow fail. Even matching non-rigid
  parent transforms that would mathematically cancel are outside this candidate.
  This is a map-admission tolerance, not a relaxation of a vertex/normal gate.
- No hierarchy is inferred. The caller supplies coherent, same-frame world maps.
  Common-parent and independent-parent rigid cases are tested. The inspected
  private routes have different direct parents; a same-parent-only shortcut
  would reject them and is not used here.
- On failure, the entire public output remains byte-for-byte unchanged, including
  failure in reverse inversion. Complete writable output may overlap inputs,
  because all reads finish before the final copy. No array is cast to a different
  struct type. Inputs must remain immutable for the call; synchronization and
  frame publication remain caller responsibilities.
- `hone_affine_f32_point(map, point, out)` applies an admitted proper-rigid map,
  checks finite results and commits three floats only on success. Input/output
  overlap is supported. No coordinate snapping, clamping, allocation, retained
  pointer, or global cache is used.

Build with binary32, no excess precision, `-ffp-contract=off`, and no fast-math or
reassociation. The header rejects incompatible float formats and fast-math.
A compiler ignoring the required contraction setting is not covered. Numerical
component equality in the oracle does not promise identical signed-zero bits.

## Independent arithmetic and identification

The implementation uses standard adjugate/cofactor inverse identities. The
binary32 grouping was identified through synthetic black-box experiments, not
through implementation-source transcription. Basis points with zero translation
isolated linear inversion and composition; origin points isolated translation;
triangle vertex and interior regions isolated reverse and forward application.

The observed order is a third-column determinant expansion with reciprocal
multiplication, explicit translation cofactor expansions, paired four-term matrix
composition, and sequential point products followed by translation. The reverse
map is the inverse of the rounded forward map, not a separately composed
`inverse(source) * target`. Algebraically equivalent groupings produced different
results. These observations concern the tested Blender binary and input domain;
they do not establish general Blender/version/compiler compatibility.

## Reproduction and results

```sh
python tools/hone/native_geometry/test_affine_f32_candidate.py
HONE_SANITIZE=1 python tools/hone/native_geometry/test_affine_f32_candidate.py
python tools/hone/native_geometry/test_affine_f32_blender_oracle.py --blender /path/to/blender
```

The contract test passes at O0/O2, including non-finite values in every map slot,
nulls, scale/shear/reflection rejection, intermediate and reverse-stage overflow,
unchanged failed output, exact/partial aliasing, and fast-math compile rejection.
The same test passed Linux ASan/UBSan independently.

The public oracle uses only synthetic objects in a new factory-startup process.
Blender 4.3.2 matched all 3,072 components for 1,024 independent-seed transforms:
common or independent parents, arbitrary rigid rotation/translation, and vertex
or interior nearest regions. Interior nearest-point evaluation is a black-box
oracle operation; this test does not validate a native closest-point algorithm.
Earlier separate diagnostic experiments also matched 4,200 reverse components
and 600 forward components. No blend file is saved.

Host compiler for Windows verification: UCRT64 GCC 15.2.0, O2, contraction off.
Its `-fstack-usage` report for exported wrappers was 240 bytes for `relative`,
240 bytes for its uninlined inverse helper, and 80 bytes for `point`. The two
240-byte frames can be active together; these are host compiler reports, not
ESP32 stack measurements. Target cycle cost, RAM placement, compiler numerical
behavior and the 20 fps budget remain unmeasured. The validating point helper
checks the matrix on every call; no per-vertex performance benefit is claimed.

## Full-frame gate remains open

Private diagnostics confirmed that this C implementation's maps equal the
independently derived diagnostic maps. With exact evaluated source and target
positions, zero-offset wrap positions match the reference in all four poses.
With the original nonzero offset, three poses match positions exactly; one
near-surface vertex in the fourth pose still differs by about `1.1921e-5`.
All four resulting diagnostic normal maxima are below `1e-4` (worst about
`6.4731e-5`), but that does not satisfy the outstanding position gate.

The residual reproduces in a static geometry clone with exact world maps and
also after moving the clone to target coordinates. The query and zero-offset
nearest hit match; the nonzero-offset response differs. A synthetic tilted-plane
near-surface test reproduces the difference. Its stabilizing behavior is not yet
identified, so no guessed threshold, face-normal substitution or tolerance
increase has been introduced. Native rigged-target rounding also remains
unresolved, and full native normals still exceed `1e-4`. Do not wire this header
into the default path on the strength of affine-only success.

A separate downstream cost remains: the material base-color experiment currently
has 39 channel programs, 1,275 instructions total, up to 207 instructions per
channel and up to 828 bytes of float scratch payload. Three evaluations per
fragment have not been timed on target. This affine work does not resolve that
cost or the frame-wide RAM budget.
