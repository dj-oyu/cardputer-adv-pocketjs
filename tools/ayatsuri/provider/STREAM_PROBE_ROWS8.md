# Synthetic depth-row batching comparison

Variant `ay_probe_rows8` enables `CONFIG_AYATSURI_STREAM_PROBE_DEPTH8` and the
existing synthetic CPU probe. Both remain default OFF. The reference variant
`ay_stream_probe` continues to use one scratch row. No real-model asset, material,
actor invocation or LCD/DMA work is added by this comparison.

The existing `ksn_mesh_render` supports depth scratch of either one or eight rows.
This experiment changes only the probe's caller-selected row count and workspace
capacity. The generic raster arithmetic, coverage, depth ties, material mask,
color interpolation, packet validation and geometry scope are unchanged.
All probe translation units receive the same compile definition to preserve the
workspace layout. Other values are rejected at compile time.

The reference loops over 135 scratch strips for a full frame, versus 17 in the
eight-row case. Each strip revalidates every packet and rebuilds every triangle's
area, inverse and edges, including triangles not intersecting that row. Batching
reduces these repeated setup passes, not the frame's pixel coverage or the
per-covered-pixel expression. The recorded reference synthetic raster average
was 325.807 ms, but no specific proportion of that time is yet attributed to
setup. Total speedup must be measured; a 7.94x frame-time speedup is not promised.

Host workspace counts are 30,144 bytes (one row) and 36,864 bytes (eight rows),
an additional 6,720 bytes; arena and observed packet snapshot remain 1,960 and
1,464 bytes. Physical internal-memory allocation, largest block and stack remain
device checks. The probe already logs those values; added `CONFIG depth_rows`
and `PIXELS depth_rows/final_hash` lines make the selected implementation and
pixel checksum explicit alongside firmware ELF identity.

`test_stream_device_probe.py --out <new-dir> [--sanitize]` builds O0/O2 with each
row count. All 32 public synthetic frames are compared pixel-by-pixel between
the reference and eight-row raster, including the last short band. Per-frame
hashes must also match across all four builds, and the established final hash
must remain `b0d19ffd`. Existing failure/rollback and freed-arena checks remain.
Host timing uses a mock clock and is not performance evidence.

Broker comparison should use the same source head for rows1 and rows8, verify
the configured rows and ELF identity, retain geometry/raster times separately,
and compare hash, rollback count, heap restoration, largest internal block and
stack high-water marks. Neither CPU-only result establishes real-model 20 FPS,
actual materials, or display DMA/ACK timing. Restore the prior authorized firmware
through the broker after the diagnostic run.
