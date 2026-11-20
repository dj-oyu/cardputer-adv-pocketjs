# Explicit binary32 frame boundaries (experimental, host verified)

Two opt-in helpers isolate independently identified arithmetic contracts. Neither
is registered in the default provider, driver evaluator, or firmware. The work
used synthetic Blender black-box observations and independent mathematics; no
Blender implementation source or private model data is included here.

## Driver input adapter

`hone_driver_input_f32` converts a **resolved and explicitly admitted** numeric
input bundle from double to binary32 and back to double before expression
evaluation. Expression arithmetic remains double. Binding identity, order, input
type admission, and immutable frame ownership remain caller responsibilities.
Do not apply this adapter to arbitrary inputs merely because a driver exists.

Blender 4.3.2 synthetic SINGLE_PROP drivers read custom double properties which
retain their original double values. For 2,048 pairs and six arithmetic, branch,
min, and trigonometric expressions, evaluating with binary32-rounded variables
then storing a binary32 result matched all 12,288 Blender outputs exactly.
The suite includes six branch-boundary/signed-zero input pairs. Evaluating directly
with the original doubles differed in all six expressions, including branch
selection on values adjacent to 0.1.
Fixed values exactly representable in binary32 do not expose this distinction.

The adapter validates every input before writing. NaN, infinity, values outside
finite binary32 range, size overflow, and partial overlap fail with unchanged
output. Exact in-place conversion and disjoint double arrays are supported.
The zero-length case accepts null spans. Signed zero and ordinary binary32
subnormals are preserved under the stated host floating-point environment.
No pointer is reinterpreted as a differently typed array.

## Near-surface offset helper

`hone_offset_f32` takes a query, an already selected hit, that triangle's geometric
unit normal, and a signed offset in the **same target-local space**. It does not
perform a nearest search, choose ties, calculate normals, or establish modifier
order. A caller must retain those contracts separately.

The black-box identification varied distance, signed offset, surface region,
translation along each axis, and tilted normals. Translation of the same
geometry exposed a coordinate-dependent transition which a distance/offset-only
hypothesis could not explain. The candidate mixes the query-to-hit direction and
oriented triangle normal near the surface using the hit's absolute-coordinate
sum plus offset magnitude. The identified transition scale is 1e-4 of that sum.
Distances below FLT_EPSILON return the hit; the exact endpoint takes the offset
path. Two adjacent floats on each side and the endpoint were checked separately.
These are observed candidate semantics, not adjustable accuracy tolerances.

Inputs and all arithmetic must remain finite. The supplied normal must have
squared length within 1e-5 of one (an input admission check; it is not renormalized).
Overflow or invalid input fails before publishing any output. Float input/output
aliasing is safe because all results are staged locally. The helper has no heap
allocation or retained cache. Round-to-nearest binary32, disabled FP contraction,
and no fast-math are required. Target behavior still needs target verification.

The initial 192-case synthetic diagnostic agreed exactly wherever its query/hit
distance was nonzero. A separate seed with 2,048 arbitrary translated triangles,
vertex/interior queries, both sides and signed offsets produced 2,035 exact
outputs and 13 differing outputs, maximum component error 1.1920928955078125e-7,
when supplied Blender's polygon normal. The public regression asserts that
measured envelope for this fixed synthetic suite; it is **not** a guarantee of
arbitrary-mesh equivalence. Selected triangle normal rounding remains relevant.

## Reproduction

```sh
CC=cc python tools/hone/native_geometry/test_f32_frame_boundaries.py
HONE_SANITIZE=1 CC=cc python tools/hone/native_geometry/test_f32_frame_boundaries.py
CC=cc python tools/hone/native_geometry/test_f32_frame_boundaries_blender_oracle.py --blender /path/to/blender
```

The contract runner checks O0 and O2, invalid inputs, output rollback, alias rules,
overflow, signed zero, binary32 rounding ties, and the near-distance endpoint.
The Blender runner compiles the same headers, constructs synthetic data in a
fresh factory-startup process, and never loads or saves private assets.
Verified oracle: Blender 4.3.2, hash 32f5fdce0a0a. Host compiler: GCC 15.2.0 on
Windows; Linux ASan/UBSan also passed the contract runner at O0 and O2.

## Full-frame evidence and remaining gates

The separate private full-scene experiment progressed from six failed normal
poses out of 63 to four after offset identification, then zero after the explicit
input adapter. Replaying the original double control values into Blender while
using these two public helpers natively passed all original 1e-4 position and
corner-normal gates: maximum position component error 2.384185791015625e-7;
maximum normal component error 7.146596908569336e-5. The suite contains four
simultaneous-input poses, each input varied independently, and eight mixed poses.
It is a bounded test set, not general compatibility proof.

Offset-stage ablation with exact source/target/world inputs made all previously
failing wrap positions exactly match Blender. Residual failures from native
world and rigged target inputs both disappeared with the input-boundary adapter.
The final public-header integration is byte-identical to the preceding private
prototype. The same 63-pose source passed Linux O1 ASan/UBSan, with position and
normal output files byte-identical to Windows. Sanitizer success alone is not
an accuracy gate.

The same public-header source was connected to the isolated streaming candidate
for those 63 poses plus the initial rest frame. Across 64 frames, all 231 mesh
copies per frame produced packet bytes and raster pixels identical to the
ordinary-arena path. 256 injected provider/capacity failures preserved the ready
snapshot. After both arenas and source geometry were freed, ready raster output
remained identical. The host instance arena decreased from 337,140 to 27,496
bytes; maximum observed packet snapshot was 35,280 bytes. These numbers exclude
other geometry buffers, double-buffer snapshots, raster scratch and device
allocation overhead; they are host allocation counts, not measured physical
SRAM/PSRAM placement or a complete target RAM budget. The same streaming source
also passed Linux ASan/UBSan for all 64 frames and 256 failure injections.

The full-scene probe still uses sampled cold rig-to-mesh transforms, restricted
root-rig rotation handling, and placeholder materials. It does not establish
arbitrary animation, full material appearance, production admission, target RAM
placement, physical DMA safety, or target-model FPS. Those gates remain separate.
