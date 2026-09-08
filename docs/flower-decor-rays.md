# Flower decorative god rays

Up to four transient, unequal warm openings sit on the shoulders of the existing main
shaft, diverging from a common offscreen source at (240,-90) with distinct
incident angles. They remain straight and widen toward the foreground.
A broad, softly attenuated penumbra separates them from it. Trunks, canopy and flowers
occlude the new layer. The main core (half-width / 2 minus six pixels) is untouched; the new
field never changes the shaft geometry, particle lighting or swarm state.

Each of four staggered opportunities hashes a new side, incident angle, width
and 18..26-second lifetime every 32 seconds, with a one-in-four chance of
skipping it. Four-second smooth fades and dark gaps hide replacement. Thus the
visible count varies rather than exposing two permanent tracks. Both light and
shadow fade together. A smooth 24-pixel transition starts six pixels further
inside the previous protection boundary, allowing the fringe to graze the main light.

All openings move right at 0.5 pixel/second with subpixel centres, with no
oscillating positional noise or inherited main-beam wobble. A shared advected
value-noise field controls density along the shafts. The final 48 pixels of each
volume fade smoothly to zero at a per-opening endpoint between rows 70 and 101;
the bottom 34 rows receive no decorative light or shadow.

Selected openings reveal from the top over 4..6 seconds with a 24-pixel soft
front. Selection probability falls linearly from 75% at base radius 18 to 9.4%
at radius 25. Unreached rows skip the pixel pass entirely.

A separate occlusion opportunity becomes more likely with width (7.8% to 62.5%
selection); half the eight-second opportunities are skipped. A selected event
has two smooth 0.75-second pulses, limited to a six-pixel-wide, 18-row fragment.
Its upstream two-row lip catches light; the downstream fragment attenuates only
the auxiliary ray's own light, never painting a dark mark over the main ray.
No new image pass or persistent buffer is used. `GARDEN_DECOR_EVENTS=0` disables
these two events for performance comparisons.

The base radius is 18..25 pixels, widens with depth, and breathes by up to three
pixels with the shared moving density field. Subpixel width arithmetic avoids
whole-pixel jumps while the centre continues its steady one-way drift.

Broad smoothstep profiles replace the narrow slit and hard shadow seam. Shadow
attenuates the existing RGB565 channels by the same small Q8 factor instead of
painting a blue-grey stripe. In-scattering follows each pixel's already main-lit
RGB with a small warm bias, so overlaps inherit the local illumination rather
than receiving a fixed RGB addition. It is added before a single spatially
dithered quantisation. This is a bounded integer transmission approximation,
not a volumetric transport simulation.

Noise is sampled per row, not per pixel. Each active row visits at most 4 x 203 pixels
before screen clipping and main-core rejection, and rows 101..134 return no light;
the pixel loop uses bounded integer arithmetic and RGB565 blending. No heap,
persistent state, extra image buffers or raymarching are added. Actual device
frame time still needs measurement; host timings are not ESP32-S3 timings.

`GARDEN_DECOR_RAYS=0` disables the layer for comparisons. The existing
`test_garden.c` keeps its original light/SIMD statistics with this switch off.
`test_garden_decor.c` separately checks 256 moments over 128 seconds: row guards,
determinism, wrap equality, unchanged main core/state and lower rows, monotonic
advection, colour-following in-scattering, visible shoulder overlap, and both light and shadow.
It also writes `.cache/garden-decor.ppm` with four preview moments.

## Sources and adaptation

- [NVIDIA GPU Gems 3, chapter 13](https://developer.nvidia.com/gpugems/gpugems3/part-ii-light-and-shadows/chapter-13-volumetric-light-scattering-post-process):
  atmospheric occlusion gives shafts their boundaries. Only that visual principle
  is used here; its GPU multi-sample radial blur is not implemented on the MCU.
- [The Book of Shaders, fBM](https://thebookofshaders.com/13/): combining a small
  number of noise octaves adds detail; noise can also warp the sampling domain.
  The earlier two-octave variation was replaced after device review by a shared
  single advected field; avoiding independent positional noise keeps the flow coherent.
