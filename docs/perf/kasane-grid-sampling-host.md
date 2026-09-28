# Fixed-window sampling: host evidence

The 240×135 source to 112×63 window has a scale factor of about 2.14 on
each axis. The current `registerResizeSource` stream reads two complete RGB565
rows whenever the bilinear plan's vertical base changes, then serves output
spans from that pair. `tools/kasane_contract/run_resize_sampling.py` compares
that **real** `ksn_grid_resize_span` kernel against the STRETCH nearest
coordinate rule and an independent box-area reference. It splits each output
row into seven 16-pixel spans, as a renderer might. No application or scene
name affects the algorithms.

Run on the host with `python tools/kasane_contract/run_resize_sampling.py`.
The runner compiles with `KASANE_PROC_DEVICE_PROBE` to count 8-pixel blocks and
`KSN_GRID_PIE_MODEL` to make PIE dispatch eligible. Host arithmetic remains
scalar; `dense_pie_eligible` is a dispatch count, **not measured hardware PIE
execution or time**. The existing arbitrary-ratio host regression also passes
with `python tools/kasane_contract/run_proc_grid_resize.py`.

| Synthetic content | Source rows: nearest distinct / bilinear provider calls / area intersections* | Mean absolute RGB error, 8-bit equivalent: nearest / bilinear | Bilinear blocks: copy / sparse scalar / dense PIE eligible |
| --- | ---: | ---: | ---: |
| Constant fill | 63 / 126 / 189 | 0.000 / 0.000 | 882 / 0 / 0 |
| One-pixel strokes | 63 / 126 / 189 | 6.633 / 5.055 | 732 / 122 / 28 |
| Smooth RGB565 gradient | 63 / 126 / 189 | 0.620 / 0.747 | 0 / 409 / 473 |
| Alternating checkerboard | 63 / 126 / 189 | 126.933 / 33.573 | 0 / 0 / 882 |

The actual STRETCH span rule requests **504 source spans / 14,490 pixels** in
this 16-output-pixel grouping. Those spans touch 63 distinct source rows; 63
is not its provider-call count. The bilinear stream requests **126 complete
rows / 30,240 pixels**, or four times fewer provider calls but over twice as
many source pixels. This difference matters when provider setup and pixel
generation have different costs.

*Area rows are the number of row intersections needed if computed separately
for each destination row. A bounded multirow cache could reuse overlaps, but
the current stream only has a two-row pair; no area kernel was implemented or
timed here. A full source row is 240 RGB565 pixels, so the area upper bound
is 45,360 source pixels. Provider calls and row generation can be much more
expensive than fetching an already resident array; this harness counts calls
and pixels rather than extrapolating device milliseconds.

The error reference integrates the covered source-pixel area using exact
integer overlap weights, then compares each output channel before output
quantization. RGB565 channels are scaled to a common 0–255 range for the
reported mean. The checkerboard demonstrates aliasing from nearest. The
gradient result also shows that a simple edge-density threshold cannot
guarantee that bilinear is closer to an area average: quantization and the
sample phase matter. For constant content, bilinear's copy route avoids the
math but still pays for two provider rows.

These results support keeping sampling as an explicit generic policy choice
(`nearest`, `bilinear`, eventually `area`) at registration rather than choosing
from an application name or a single scene-wide edge score. A bounded
automatic selector would need a documented quality/cost objective and tests
over temporal changes, not just these four still frames. The current shipping
default remains bilinear. `registerResizeSource` now accepts an explicit
`sampling: 'nearest'|'bilinear'` option. The nearest image port uses the same
integer pixel-center mapping as STRETCH but can request a contiguous span
larger than the renderer's 32-pixel scratch. The QuickJS host test counts 441
provider calls and 14,616 source pixels for this window with 16-pixel output
spans, verifies every output pixel, and retries after a provider I/O failure.
This is a host access count, not a device timing result.
