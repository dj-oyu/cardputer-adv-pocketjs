# Per-row span narrowing for ray_row (measured badly once, not dead)

Preserved 2026-09-08 after `git checkout -- main/scene/flower.c` discarded it.
It first went to `.cache/preserved/`, which is gitignored and would not have
survived `git clean -xdf`; it is tracked here instead, which is the only place
on this machine that actually is durable.

## What it is

Substituting b and c into `d = b*b - q2*c` leaves a quadratic in `dx` whose
three coefficients are constant along a row, so the columns where an ellipsoid
can be hit are one interval, found with a single sqrt per petal-row instead of
a discriminant per pixel.

`rejection.c.frag` replaces the `for(int x=p->xmin;x<=p->xmax;x++)` line in
`ray_row`. Bells keep the full span: `bell_hit` intersects six conical bands and
its miss set is not one interval in dx.

## What was established, and is not worth re-establishing

- **Bit-exact.** 280 frames x 7 species, 0 differing pixels, max channel step 0.
  The per-pixel `d<0` test still runs; the interval is widened one column each
  side so float rounding in the closed form cannot drop a column the exact test
  would have kept. `test_bitexact.c` is the harness and it is the expensive part.
- **Removes 27-50% of per-pixel visits** (`measure_visits.c`): snowdrop 50.2,
  valley 43.3, sunflower 40.4, daffodil 39.6, crocus 35.8, tulip 33.3,
  calla 27.0. One sqrt buys 5-7 removed visits.
- Ellipsoid `d<0` misses are 51-72% of visits; bell misses 59-64%.

## Why it lost, and what would change that

On device it cost 20 ms of `kernel=` (66.1-67.1 -> 84.7-87.2, fps 11.6 -> 9.5).
The visits it removes are the cheap ones -- the ones that fall out on `d<0`
immediately -- and one sqrt plus one reciprocal per petal-row cost more than
they did. On x86 it was a wash, which is why the host could not decide it.

It was measured against a specific balance, and two parts of that balance are
changing:

1. 34.5 ms of garden pixel loop sat in front of it. After the PIE kernel that
   becomes ~3 ms, so ray_row's share of `kernel=` roughly doubles.
2. `ray=` did not track (it stayed flat while `kernel=` rose 20 ms), so the
   30-42 ms figure it reported was wrong; the corrected estimate is ~13 ms.

Re-measure with `kernel=` alone, which has never contradicted itself. The
number to beat is a fall in `kernel=`; anything else is noise at this size.

## Measured 2026-09-08, against hits rather than visits: it is worth ~0.2 ms

Both sides of the paragraph above are wrong, and the arithmetic that settles it
did not need the board.

`ray_row` was measured at 30.25 ms with **6,600 visits and 1,957 hits** a frame,
so a hit costs about **3,400 cycles** and a `d<0` miss costs about **18** -- the
miss path is `dx`, `b`, `c`, `d` and a compare, all hardware-FPU multiply-adds
with no sqrt in them. Removing 27-50% of the visits therefore removes

    6,600 x 0.4 x 18 = 43,000 cycles = 0.18 ms

out of 30 ms. **A reduction in visits is not a reduction in time.** The visits
this file removes are the cheapest pixels in the function, and the time is in
the 1,957 that are not removed at all.

The 20 ms it was measured to *cost* does not survive either. `measure_visits.c`
counts 186-1,178 ellipsoid part-rows a frame; the fragment adds one `sqrtf`, two
float divides and two roundings to each. Even at a pessimistic 1,000 cycles that
is 1.7 ms, not 20. So the old device figure is out by a factor of ten and the
explanation attached to it ("one sqrt cost more than the visits it removed") is
right in direction and out by two orders of magnitude in size: the real trade is
about 0.2 ms against about 0.3 ms.

Both numbers came from the `ray=` counter during the period when it was known
not to track. Nothing here is worth re-running; it is kept because the closed
form is correct and someone may want it for a different reason.

The include paths were also broken -- `../main/` resolved to `tools/main/` after
this directory was created around the files, so neither program had been
runnable since the day they were preserved. Fixed, and `measure_visits.c` now
also reports part-rows, which is the number that made the cost side decidable.
`test_bitexact.c` still does not build: it includes a generated
`flower_ref_body.h` that was never preserved with it. Left alone, because the
thing it would prove -- that the narrowing is bit-exact -- is not in doubt and
is not worth 0.2 ms.

**Where the time actually is:** `shade()` and the two software `sqrtf` calls per
hit. `sqrtf` on this target resolves to an 88-instruction Rust routine behind a
two-level call, and `normal()` adds a soft-float divide to it.
