# Near-surface offset diagnostic (host only)

`diagnose_wrap_offset_near_plane.py` is a reproducible black-box experiment,
not a passing native-wrap equivalence test. It constructs two synthetic triangles
in a fresh Blender process, never loads or saves a model, and needs no compiler.
It exercises both sides, positive/negative offset, eight distance scales, and
several offset magnitudes: 192 cases, with repeated oracle evaluations per case.

Run from any host with a Blender executable:

```sh
python tools/hone/native_geometry/diagnose_wrap_offset_near_plane.py --blender /path/to/blender
```

Observed with Blender 4.3.2 (32f5fdce0a0a): all 192 cases completed; repeated
nonzero-offset evaluation before/after zero-offset evaluation was identical;
all measured outputs were finite. `DIAGNOSTIC_COMPLETE` only means the diagnostic
completed. No native geometry admission, accuracy threshold, or target speed is
established by this message.

The experiment takes the nearest point from the modifier itself at offset zero.
That point also matched the mathutils closest-triangle helper for these cases.
It then compares independent binary32 constructions: normalize then scale,
scale by offset/length, endpoint interpolation, and query-based displacement.
For the tilted triangle, a signed-plane expression is also reported strictly as
an interior diagnostic, not an edge/vertex fallback.

At offset 0.0025 on the positive side of the tilted plane, the normalize-then-scale
construction had maximum component residual about 2.38e-7 at distance scale 1e-4,
8.64e-6 at 1e-5, and 3.82e-4 at 1e-7. Changing those arithmetic orders does not
remove the mismatch. The signed-plane construction reduces these residuals but
is not exact and is not adopted.

The axis-aligned vertex case separates arithmetic from near-surface behavior:
query (-1e-7, -1e-7, 1e-7) has an exactly zero nearest point. Offsets 0.0001 and
0.001 match normalize-then-scale, but offsets 0.01 and 0.1 produce tangential
components of magnitude approximately 0.001 while moving farther along the face
normal. A simple rounding-order change cannot explain that offset-dependent
change of direction. At the smaller query scale 1e-8 the observed result is the
nearest point for this case. These observations do **not** identify a general
transition rule or justify introducing a fitted threshold into native code.

## Integration status

The broader host experiment remains experimental: 57 of 63 independently varied
or mixed poses passed the existing position and corner-normal gates; six failed
the normal gate, while all passed the position gate. Four simultaneous-input
poses alone had passed and are insufficient evidence for general admission.
Two failing poses remain failing even with oracle source positions, target
positions, and transforms. In four other failing poses, exact oracle inputs pass,
while native target and native world inputs each expose further residuals.
Replacing only source morph positions did not change those failed outcomes.
With exact final positions the normal kernel agrees closely, so weakening its
normal gate would conceal upstream position errors.

The same frozen full-scene v3 source was also run for all 63 poses on Linux at
O1 with AddressSanitizer and UndefinedBehaviorSanitizer: it completed without
sanitizer findings. Its position and normal output files were byte-identical to
the Windows host run (maximum component delta 0 for both). This checks that
host execution, not accuracy admission: the same six normal failures remain.

The diagnostic adds no production fallback or threshold, changes no default
provider path, and makes no general Blender-compatibility claim. Root palette
and world-rounding candidates still require their stated restricted domains;
current full-scene tests still use sampled cold rig-to-mesh transforms and
placeholder materials. Target RAM, DMA behavior, and model FPS remain unmeasured.
