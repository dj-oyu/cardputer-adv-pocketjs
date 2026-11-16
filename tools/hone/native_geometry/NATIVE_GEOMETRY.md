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
