# Public synthetic blink contract candidate

Pure Python, deliberately unrelated rectangle geometry. No private model, original material formula, Blender asset or product changes.

Run `python test_blink_contract.py` from this directory. 216 normalized Both/left/right combinations, 19,440 spatial mask comparisons and seven invalid inputs pass. Pure scalar selection/interpolation oracle; not an independent renderer or proof of runtime implementation.

Each side is a rectangle centered at x=-3/+3. The morphable upper/lower lid endpoints are y=+/-(1-b), b=max(Both,side). A binary mask accepts |x-center|<=1 and |y|<=1-b; visibility is b<0.98. Binary material is an arbitrary test fixture, NOT the authored fox shader. No normal, DQ, alpha blending, depth or GPU behavior is modeled.

Controls must be finite normalized numbers; booleans rejected. Evaluate into a new result. The immutable prior-result checks demonstrate only this Python function behavior; no product OOM, transaction, ownership or native pending-render guarantees are inferred. Runtime adapter should validate all controls before touching a pending packet and capture morph/mask/visibility from the SAME explicit-time state.

Runtime tests still required: left-only/right-only controls, reverse seek, pause, reopen, mask coverage/depth, rejected/OOM input retaining previous packet, disposal and source mutation while rendering pending. Keep control precision before the visibility comparison: rounding .979999 to 8-bit first flips visibility at .98. Quantization policy is not silently approved by this fixture.

This candidate is not quality acceptance of any model and does not measure 20fps. Independent review pending.

## Proposed adapter boundary (not implemented)

Schema `public-synthetic-blink-v1`; side order [-1,+1]; right-handed XY plane, Z=0, arbitrary units, centers(-3,0,0)/(3,0,0). Evaluate max and `b < 0.98` once in the JS Number (binary64) control domain, then transport the resulting visibility bit with that same frame's morph and mask parameters. Do not recompute visibility from a Float32 or U8-rounded blink. Finite normalized validation precedes output allocation/commit. Python float oracle corresponds to binary64 intent; port tests must still verify JS values explicitly.

Float32 geometry/mask buffers are a separate precision domain. Capture both unrounded b and transported Float32 values in the harness. For exact raster boundary samples, report representability/classification differences rather than widening a visual tolerance or claiming identical alpha coverage. Native alpha/depth/blending design and its error bounds are not defined by this candidate.
