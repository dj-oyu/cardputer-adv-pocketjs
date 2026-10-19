# Bounded runner screen affine comparison (opt-in)

This separate comparison follows normal-OFF PR8 (`f7d86c14`, tree
`11ab870b`), not the three frozen diagnostic heads in PR13/14/15. It changes
no race state, perspective division, sqrt, camera, sort or submission order.
`POCKET_DERBY_RUNNER_AFFINE` defaults OFF. ON substitutes only the built-in
`derby.runner` body/jockey geometry; arbitrary VM plans and typed points keep
their existing path. Legs retain the eight draw arguments and `lroundf`.

The prior separate [Q14 boundary experiment](https://github.com/dj-oyu/cardputer-adv-pocketjs/blob/24a2c23b1a22c06698e8a1d8d1029ae71c16999c/docs/apps/derby-q14-boundary-experiment.md)
used synthetic exact-lattice points, measured no host end-to-end gain and
did not replace game geometry. This candidate reuses the same existing
Kasane scalar/PIE kernels; it does not repeat or relabel that experiment.

## Numerical boundary

- Input is the existing float screen-space x/back-y/scale/hoof/leg0..3 after
  the ordinary JS double perspective calculation. It is not world Q14.
- Nine unique body/jockey vertices have integer local coordinates scaled by
  16: x in [-16,80], y in [-32,16]. The affine coefficient is
  `round(S*2560)` (Q2.14 represents `2.5*S/16`), diagonal, no shear.
- S is finite in [0,8]. Coefficients are therefore [0,20480], within Q2.14.
  Translation is int32 Q14 from screen x/y plus 8192 for explicit half-up
  pixel rounding. This differs from the VM's negative-half ties away from
  zero. The kernel still computes floor of the wide sum then sat16.
- The pre-round geometry must stay inside [-479.99,719.99], and the existing
  hoof/leg coordinates inside [-480,720]. Otherwise the original VM runs.
  No frame is written on rejection. Saturation cannot occur in the admitted
  domain. The largest sum magnitude is below 2^24 (signed 25 bits); actual
  scalar accumulation is int64 and hardware PIE uses 40-bit QACC.
- Coefficient and translation quantization contributes less than .003 px
  before pixel rounding. Against the actual float VM, the acceptance bound
  is one pixel per endpoint, including float operation-order differences.
  Pixel-identical output is not claimed.

PIE processes eight vertices with one common transform plus one scalar tail;
eight horses do not share a transform. The threshold defaults to **0**, so
the candidate uses scalar until device calibration supplies a positive
`POCKET_DERBY_RUNNER_AFFINE_PIE_MIN`. Host `pie_batches` stays zero; the
separate arithmetic-model counter is `pie_model_batches`.

## Reproducible host comparison

`python -B tools/games/run_derby_runner_affine.py --full --out artifacts/affine`

The current generated runner ROM instructions are the reference. Tests cover
20,012 admitted boundary/random cases, invalid and nonfinite inputs, scale
and half-tie boundaries, output untouched on rejection, actual scalar/model
parity, and real pose/pj/rin expressions over 38 continuous trajectories
(106,346 inputs). Fixed camera positions track a moving aim; straight/oval,
three PAN cameras and three tiers are included. Extra sweeps cross Z=.02 and
the S=8 fallback boundary, and pan a near-static outline by subpixels. These
are deterministic host fixtures, not new camera features or target samples.

Temporal comparison counts ordinary float pixel movements separately from
candidate-only toggles and float movements delayed by the candidate. It
compares successive endpoint deltas; the one-pixel static bound allows up
to two pixels of delta difference. The measured sweep currently reaches one.
The candidate-only toggles are real differences requiring review; wireframe
does not make quantization invisible. Near-plane visibility remains the
unchanged Z>.02 predicate before this transform, and rejected inputs use
the original VM. Close rails are an unchanged control: only `derby.runner`
is substituted; the full adapter test retains all original non-runner VM,
clipping, band/full renderer and screen-face checks across all three tiers.

`result.json` records input ranges, endpoint/motion differences, sampled real
Kasane raster differences and complete VM versus complete candidate boundary
host costs (validation/conversion/dispatch/segment assembly included; shared
JS argument reading and frame merge excluded from both arms). Timing never
gates CI. `worst.png` shows float VM | candidate | changed pixels in magenta.
Optimized and ASan/UBSan runs are separate; sanitizer timings are not a speed
comparison. Device acceptance needs same-context S3 measurements, conversion
and dispatch costs, real scalar/PIE counts, memory and temporal visual review.
No current CPU-cycle share, fps improvement or target adoption is asserted.

No MCP source fetch/build/flash is needed or authorized for these host/CI
tests. Cached source remains unchanged. This is a Draft comparison only.
