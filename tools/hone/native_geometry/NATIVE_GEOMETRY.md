# Native geometry candidate

`native_geometry.py source.private.json geometry.private.h` emits immutable C
arrays from `hone.source-preservation.v1`, using the companion corner planner.
It writes only a new output and reports input/output SHA-256. Keep both the
source and generated asset private. Publish only generic tools/tests.

The typed view retains corner topology, rest positions/normals, polygon
smoothing, source indices, vertex-group weight indices, and morph coordinates
with relative-key indices. Floats are explicitly rounded to IEEE754 binary32;
this is not byte-for-byte preservation of Blender's complete scene. Source
mesh/group/key names and controls remain in the authoritative source package.

This is a compile-input candidate, **not a runtime-ready model**. It does not
infer material, opacity, transforms, modifier execution, animation, normal
recomputation, ownership or LCD registration. No private broker destination or
recipe is finalized until a real consumer exists. Do not substitute the static
opaque AYNM compiler, which would silently discard unsupported semantics.

Run `python -m unittest -v` in this directory. Synthetic tests compile and run
a C11 consumer under ASan/UBSan (LSan disabled), verify layout, morph-relative
mapping and corner/weight duplication, invalid input and output-no-overwrite.
No firmware build, target memory budget or hardware performance is established.

## Relative-morph consumer candidate

`native_morph.h` reads the generated typed view, with caller-resolved values and
per-vertex masks in exact source key order. Negative/extrapolated finite values
are allowed; masks must be in [0,1]. The point result commits only on success.
No allocation, intermediate mesh copy or virtual callback is added. Cold data
must be generated/validated and immutable, and output must be disjoint scratch.
This candidate is not wired into firmware; it does not evaluate drivers, skin,
normals or material opacity. Arithmetic is binary32, not an exact Blender oracle.

Only an explicit `use_relative: true` sets the generated relative-morph flag;
the consumer rejects absolute/time-based or unknown morph modes. Values must
already account for mute/driver/slider policy, and masks must correspond to each
source key's vertex-group mask. These are not inferred from absent metadata.
The synthetic test checks 301 scalar/mask combinations against a double
reference (1e-6 tolerance), not a Blender image or deformation acceptance gate.

The consumer's finite-value and overflow rejection requires normal IEEE floating
point semantics. Do not compile it with `-ffast-math`, `-ffinite-math-only`, or
other options that assume finite inputs or remove NaN/Inf checks. Firmware
integration must enforce this for every translation unit that includes it.
