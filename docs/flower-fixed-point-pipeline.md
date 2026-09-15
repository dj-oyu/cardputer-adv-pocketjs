# Carrying the fixed point through the flower pipeline

Where this stands after the B=8 measurement (docs/flower-decor-cost.md): replacing
`sqrtf` with an integer restoration root bought nothing, and the reason was not the
loop. `d` arrives as a float and the root leaves as one, and each of those two
conversions is a call into soft-float on a part with no FPU -- the loop got nine
times cheaper and the two conversions cost what the loop used to. A fixed point
that stops at the arithmetic and leaves the interface float is a rounding error on
a frame.

## The arithmetic that decides the shape

The ellipsoid test is

    b = q4*dx + q5*dy              O(800) for the smallest petals
    c = q0*dx*dx + 2*q3*dx*dy + q1*dy*dy - 1
    d = b*b - q[2]*c               the interesting hits are the ones where d ~ 0

`b` is O(800) and `d` is O(0): near tangency the subtraction cancels, so `d` has to
carry roughly 20 bits of *relative* precision on a quantity whose fixed-point scale
would have been chosen for 800^2. That is around 40 bits of dynamic range, and a
32-bit word does not hold it. This is why "convert the ray maths to fixed point"
cannot be a change of types: the discriminant needs a representation with an
exponent, and a plain Q-format word is not one.

The same argument, smaller, applies to the depth comparison that consumes the root:
`z = (-b + root) * invzz + c.z` subtracts two nearly equal numbers for exactly the
hits that matter.

## What is in place

`main/scene/fixed_sqrt.h` -- a square root with no float on either side. It takes a
bare integer radicand and returns a 15-bit mantissa plus the power of two it belongs
to, `sqrt(x) == r * 2^shift`, by normalising the radicand's leading bit to position
29 or 30 (shifting up for small values, where a bare integer root loses everything:
`floor(sqrt(3))` is 1 against 1.732). Verified in tools/flower_isqrt_norm_test.c:

    16-bit domain (every x)          worst relative error 6.1e-05   14 exact bits
    32-bit domain (log sweep + rnd)  worst relative error 6.1e-05   14 exact bits
    Q14 output                       worst relative error 6.1e-05   14 exact bits
    monotone over x <= 2^20          yes, 0 violations

The point of the numbers is that they are the *same* at every magnitude: the
dynamic range lives in the exponent and costs a shift, which is what the geometry
needs and what a Q-format word cannot do.

## The pieces that follow, in the order that keeps a measurement possible

1. **The discriminant in normalised form.** `b*b` and `q[2]*c` each as (mantissa,
   exponent), subtracted after aligning exponents -- the same trick the root uses.
   The depth buffer stays in whatever this produces, one exponent per row of parts
   if a per-pixel one is too wide.
2. **The vector layer in Q14.** `dot` of two Q14 vectors is three products of
   <= 2^28 that sum to <= 2^30, so 32 bits hold it; `normal` is that dot and the
   root above, no division by a float. This is the layer `shade()` needs.
3. **`shade()` whole, not partly.** Its outputs are integers (rgbd takes ints) and
   its inputs are bounded (unit normals, colours <= 255), so every branch --
   corona, gold, leaf, filament -- converts. What it must not be is a float
   function with two fixed-point islands: that is the B=8 result again.
4. **PIE after 1..3, or beside them for the garden.** The mantissa the root returns
   is 15 bits and the lane analysis (tools/flower_sqrt_lanes.c) says a 16-bit lane
   holds 8 significant bits of root and 32-bit lanes 16 -- the normalised root sits
   exactly in that range, so the lane version of this root is the same recurrence
   over `m`.
