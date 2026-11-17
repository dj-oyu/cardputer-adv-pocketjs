# Exact opaque occlusion collector candidate

`alpha_graph_occlusion_candidate.h` is an independent opt-in collector. Existing
renderers, four-layer collector and default firmware behavior are unchanged.
The candidate evaluates every packet with the existing fragment evaluator first;
invalid fragments are rejected even when an opaque surface hides them. The first
pass finds the last-composited fragment with alpha **exactly** 1. A second pass
retains it and only later-composited contributions. Four retained layers remain
the hard limit; no alpha epsilon, geometry deletion or threshold increase occurs.

Ordering exactly follows the existing insertion rule: increasing depth, decreasing
packet order, and for equal depth/order, decreasing input-array index. Consequently
the nearest opaque is maximum depth, then minimum order, then earliest array entry.
Output is staged locally; invalid input or retained overflow leaves it unchanged.
Inputs, shader code, drivers and graph snapshot must remain immutable across both
passes and replay. Scratch/output disjointness is the existing caller contract.

For finite RGB, any previous accumulated color is multiplied by zero at an exact
opaque fragment. Discarding those contributions therefore preserves the reference
composition. It does not skip their validation. Near-one alpha must retain its
background; the tests include `nextafterf(1,0)` explicitly.

## Verification

Windows GCC O0/O2: 4000 randomized scenes, 55140 pixel float-RGB comparisons exactly
equal to an unrestricted 32-layer reference, 8860 remaining-overflow cases rejected.
Tests include transparent mixing, depth cutoff, equal depth/order keys, near-one
alpha and an invalid shader behind an opaque fragment with unchanged output.
The existing band suite also passes O0/O2 with this collector explicitly selected:
pixel equality at 1/2/4/8 rows, nonlinear clipped coordinates, span/layer failures,
binding and alias failures, unchanged output before failed preflight.

Private restored-model front-view samples 0/3/15/31/47/62 all pass candidate full
frame preflight and every RGB565 pixel equals an unlimited composition reference.
Each originally failed the legacy collector with 22 overflow pixels, maximum six
raw layers. Candidate effective maximum is one layer in these samples. Output
hashes change for samples 3 and 62, unlike the old top-view diagnostic; each output
has four colors. This is limited unlit base-color coverage, not Blender image
parity. The associated geometry/driver sequence still runs all 63 poses, but only
these six run the new full-frame material comparison.

Run from repository root:

```
CC=cc python3 tools/hone/native_geometry/test_alpha_graph_occlusion.py --out /tmp/occlusion --sanitize
CC=cc python3 tools/hone/native_geometry/test_alpha_graph_occlusion_band.py --out /tmp/occlusion-band --sanitize
```

Private reproduction is under `.cache/preserved/normal-full-diagnosis/`:
`run-material-occlusion-candidate-six.private.py <fresh-out> [--sanitize]`, with
`material-occlusion-candidate-six-v1.log` as Windows evidence. No private geometry,
shader coefficients or material names are included in this public candidate.
The private runner's inherited payload summary says raster_preflight=false;
new `GRAPH_GATE` records are the authoritative six-pose material evidence.
Raw overflow diagnostic counts remain present even when candidate preflight passes.

## Remaining limits

This is not a firmware performance optimization result: it evaluates shaders in
two passes and can cost more CPU than the original early-reject collector. Target
stack, physical DMA and FPS are unmeasured. No target allocation increase is made
by this collector; it uses four staged retained records and constant metadata.
Full-scene lighting, near clipping, color management, camera coverage and Blender
image parity remain incomplete. It is not enabled in the resident actor or firmware.
