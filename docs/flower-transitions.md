# Flower scene transitions

## Cause and choice

`flower_prepare_rotating` previously assigned the flower-selection RNG directly
to `GardenFrame.seed`. The flower was transparent at the swap, but the trunks,
canopy and grass switched instantly. The exposed background made the cut visible.

| Option | Appearance | Cost / limitation |
| --- | --- | --- |
| Fade to black | Hides every discontinuity | Interrupts the continuous woodland atmosphere |
| Full scene crossfade | Smooth, but may show two flowers at once | A second flower trace or a retained 64,800-byte RGB565 frame |
| Shape morph | Potentially distinctive | Species have different topology, part counts and materials; arbitrary part interpolation gives implausible intermediate plants |
| Keep one forest forever | Cheapest and continuous | Does not meet the requested periodic reconfiguration |
| Blend vegetation, continuously move light | Preserves the air and hides the layout cut | Chosen: one extra vegetation pass and row blend only during transition |

## Implementation

The 40-second interval remains. At each species change the old and new layout
seeds are retained and a 3-second smoothstep moves between them. At mix zero,
the drawn forest is exactly the old layout; at 256 it is exactly the new one.
The existing 1.2-second flower dissolve also uses smoothstep.

`garden_atmosphere_row` renders main light, auxiliary rays and swarm once.
`garden_vegetation_row` overlays either layout onto that same result.
`garden_row_blend` uses one 480-byte automatic row for the second layout, with
fast paths for both endpoints. Flowers and rain retain their existing order.

The main shaft's layout-dependent horizontal offset (+/-8 pixels), half-width
offset (+/-5 pixels), and slant coefficient (+/-8, roughly +/-4 pixels at either
vertical end) use the same transition factor. These are bounds on each layout's
offset, not maximum old-to-new differences. Time-varying atmospheric noise keeps
running. Geometry is set before the swarm update, so visibility and phototaxis
read the same shaft that is drawn. Direct per-species rendering uses the default
layout and no transition, preserving its existing contract.

Storage: 12 bytes of transition state and 8 bytes in the existing shared garden
frame, plus the temporary row. No retained image, extra flower geometry, or heap
allocation is introduced. During the approximately 7.5% of time spent blending,
vegetation is drawn twice and 240 pixels/row are blended; atmosphere is not drawn
twice. Actual device frame-time impact still needs measurement.

Validation: `test_flower.c` covers 128 periodic reconfigurations, exact old-layout
start factors, all 14 species and strip equivalence. `test_garden_transition.c`
checks exact colour/geometry endpoints, bounded channel interpolation, unchanged
atmosphere where layouts agree, one-step geometry increments, row guards and
unchanged frame state. The existing garden/SIMD/swarm tests remain passing.
