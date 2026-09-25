#ifndef KSN_RENDER_H
#define KSN_RENDER_H
#include "ksn_core.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint32_t bands,transferred_bytes; } ksn_render_stats;
#ifdef KASANE_P4_DECODE_CYCLE_PROBE
/* Diagnostic only: counts decode_view's bracketed cycles and an adjacent
 * empty bracket with the same cycle reader. Neither is a product timer. */
void ksn_render_decode_cycle_reset(void);
void ksn_render_decode_cycle_report(void);
#endif
/* Per-phase render counters, the measurement boundary 7 of
 * docs/perf/kasane-opt-survey.md says is missing. Every field is a COUNT of
 * events that happened, never a duration: `_cy` sums the CPU cycle reads
 * (`rsr.ccount`, esp_cpu_get_cycle_count) taken either side of one bracket and
 * `_n` is how many times that bracket was entered. They are logged as counts
 * and are never divided by the clock in this layer, because a counter that
 * reports microseconds reports the measurement as much as the subject
 * (docs/perf/pie-simd.md 4.2, 6.5). A host build has no `rsr.ccount`, so there
 * `_cy` stays 0 and only the entry counts are available (pie-simd.md 6.7):
 * time is a device question, counts are a host question.
 *
 * The five brackets: fill = one fill565 call (a band background, or one row of
 * an opaque rect), span = one text->span, tile = one whole render_group,
 * blend = one per-pixel composite loop entered (covers+sample+blend, or the
 * group's child loop), read = one command decode the renderer asked for: a
 * ksn_core_read on its own, or, now that boundary 2a is integrated, one
 * frame_command() call, of which only the first per command per frame reaches
 * the core. The switch off makes every frame_command() call a real read again,
 * so read_n is not comparable across those two arms; the other four brackets
 * are unaffected by 2a. `tile`
 * brackets the group's composite as a whole and so contains the reads, spans
 * and blends the group makes, which is why the five sums are not additive.
 *
 * g_ksn_prof gates the reads and defaults to 0. With it off the render path
 * pays a load and a branch per bracket and no cycle read, and the pixels are
 * exactly what they were before this existed; the instrument and every
 * optimization it is used to judge go in separate commits (pie-simd.md 6.1). */
typedef struct {
    uint32_t fill_cy,span_cy,tile_cy,blend_cy,read_cy;
    uint32_t fill_n,span_n,tile_n,blend_n,read_n;
} ksn_render_prof;
extern int g_ksn_prof;
extern ksn_render_prof g_ksn_render_prof;
/* Snapshot and zero, so one log window can print its own counts. */
void ksn_render_prof_read(ksn_render_prof *out);
/* ksn_render_stats.bands is one bit per 8-row strip. The band count and the
 * number of contiguous runs are what the log line was missing: a 3.37 ms send
 * can be 7 bands or 14, and `bytes` alone cannot say which
 * (kasane-opt-survey.md 10.1, and board.c re-windows on every discontinuity). */
unsigned ksn_render_band_count(uint32_t mask);
unsigned ksn_render_band_runs(uint32_t mask);
/* Boundary 2a switch: 1 (default) decodes each frame command once per frame
 * into a renderer-owned view (KSN_COMMANDS views + a KSN_TEXT_BYTES frame text
 * pool, static); 0 reads the command again for every band, which is the pre-2a
 * reference path. Both paths live in one binary so a same-binary A/B can
 * compare read counts and frame time without a rebuild. Rendering is
 * pixel-neutral either way; only the number of ksn_core_read calls changes. */
extern int g_ksn_decode_once;
/* Rotated image spans map a destination pixel to a source column by
 * floor(u*source_width/(w*32768)) with u affine in x. With this switch on
 * (default) the quotient and remainder of that division are advanced by one
 * comparison and one conditional subtraction per pixel instead of dividing
 * twice; the source index, the block fetched and the composited pixel are
 * identical in both arms. Off restores the per-pixel rational division.
 * Owner task only; read once per rotated span. */
extern bool g_ksn_image_rotate_step;
/* Rotated spans take their two span anchors from a per-row table built at run
 * time in DRAM. The entries are the same quotient and remainder the per-pixel
 * division produces at x = base_x + 16j (the span anchors are affine in x and
 * advance by the identical carry rule), so the source index, the block fetched
 * and the composited pixel are bit-identical in both arms: on the table when a
 * row and its command still match and x is on the table's grid, off the
 * per-span 64-bit division otherwise. Off restores it for every span. Owner
 * task only; read once per rotated span. */
extern bool g_ksn_image_rotate_anchor;
/* Rotated spans: the anchors are monotone along a span (the per-pixel quotient
 * has the sign of the numerator step and the carry only adds to it), so a span
 * whose first pixel is outside the source rectangle is decided by one interval
 * test on the two span anchors - no pixel of it can be accepted when the
 * extreme pixel on the failing side is out. Such a span costs the test and
 * clearing the span scratch instead of the per-pixel work the loop pays to
 * discard each pixel. No pixel can change: the loop this replaces is the one
 * that writes those zeroes. Off restores the per-pixel test for every span.
 * Owner task only; read once per rotated span. */
extern bool g_ksn_image_rotate_reject;
/* Host diagnostics for the anchor table: entries built for the last rotated
 * row, and its starting x. The renderer never calls this. */
uint32_t ksn_render_rotate_anchor_state(int *base_x);
#ifdef KSN_ANCHOR_COUNT
/* Counters for harnesses compiled with -DKSN_ANCHOR_COUNT; absent from the
 * shipping object (they exist so a test can assert the table path is live). */
extern uint32_t g_ksn_image_anchor_builds,g_ksn_image_reject_tests,g_ksn_image_reject_spans;
#endif
/* Stretched image spans map a destination column to
 * floor((x-source_x)*source_width + source_width/2)/width) and take the
 * difference of that quotient inside a span. The numerator advances by the
 * constant source_width = q*width + r, so with this switch on (default) the
 * quotient and the remainder advance by one comparison and one conditional
 * subtraction per pixel instead of dividing twice; the source index, the
 * source block fetched and the composited pixel are identical in both arms.
 * Off restores the per-pixel rational division. Owner task only; read once
 * per stretched span. */
extern bool g_ksn_image_stretch_step;
/* Tile-level work (docs/perf/kasane-tile.md). The group's children are
 * composited into a 64-pixel scratch tile block by block:
 *   g_ksn_tile_pixels -- the block's size, 64 or 16 (survey 3b).
 *   g_ksn_tile_reach  -- exact, default on: a child's loop runs over its own
 *                        clipped x interval and a block no child can reach is
 *                        skipped whole (survey 3a).
 *   g_ksn_tile_smooth -- smooth layers: one exact anchor per block plus a
 *                        per-pixel increment instead of the per-pixel sample.
 *                        Exact where the child does not vary along x, approximate
 *                        where it does; default off until the doc's measured
 *                        step says otherwise.
 * All three are read once per render; 0 restores the path each one replaces. */
extern int g_ksn_tile_pixels,g_ksn_tile_reach,g_ksn_tile_smooth;
/* Bounded production subset: rect, round rect, 1/2 px stroke, two-color
 * horizontal/vertical gradient, font-port TEXT, source-span IMAGE, alpha and
 * isolated group opacity. The borrowed text port and its immutable resources
 * must remain replayable for pending and committed frames. One shared 112-byte
 * span scratch plus group tile/provenance (264) and provider row (up to 128)
 * fit the 512-byte pixel scratch budget. No component surface.
 * Unsupported commands are rejected before the first transfer. The caller
 * schedules retry/discard; callbacks may request invalidation, but must not
 * otherwise mutate the core or reenter JS/presentation. prepare_frame consumes
 * preexisting invalidation; later invalidation survives the acknowledgement. */
ksn_result ksn_render_rects(ksn_core *core,const ksn_display_port *display,ksn_render_stats *stats);
/* As ksn_render_rects, but load the host's replayable scene below the command
 * layers. Kept as a separate entry point so the stable display-port ABI and
 * its positional host fixtures do not grow for one composition policy. */
ksn_result ksn_render_rects_backdrop(ksn_core *,const ksn_display_port *,
                                     ksn_backdrop_loader,bool occlusion_safe,ksn_render_stats *);
/* TEMPORARY A/B switch for that renderer's coverage solver: 1 = solve a row's
 * covered x set as runs once per row per command, 0 = ask the per-pixel
 * predicate for every pixel. Both arms live in one binary and produce the same
 * pixels (test_coverage_spans.c compares them exhaustively and frame by frame). */
extern int g_ksn_row_coverage;
/* Candidate 4c switch: 1 (default) builds the row's sampled colours once per
 * row -- a 240-entry table for a horizontal gradient's ramp, one value for a
 * vertical gradient's row, one value for the shape and text commands -- and
 * reads them per pixel; 0 asks sample() for every pixel, which is the path this
 * file ran before the table existed. The ramp is the same integer expression
 * rearranged, not a rounded copy of it, so both arms must produce byte
 * identical panels: test_row_table.c checks them pixel by pixel over the whole
 * 240x135 panel for 120 frames, and against `interpolate` over a sweep. The
 * table is only consulted where a row's covered window has been solved, i.e.
 * under g_ksn_row_coverage = 1; with that off there is no window and the
 * predicate path samples as it always did. DRAM cost: one 960-byte table in
 * .bss, against a budget of ~334 KiB (CLAUDE.md). */
extern int g_ksn_row_table;
/* Boundary 4's quantized-key table for the direct blend chain (ksn_render.c,
 * "Boundary 4"): the per-pixel chain is a function of the destination's channel
 * value, the command's sampled colour and effective alpha, and the bayer
 * threshold, so each of those costed once per command becomes a row of 32/64
 * values. g_ksn_blend_lut = the solid arm (RECT/ROUND_RECT/STROKE, and a
 * gradient whose from == to), exact, default 1.
 * g_ksn_blend_lut_alpha = the text arm, whose parameter (the coverage-driven
 * effective alpha) is quantized to 16 levels, default 0 because that arm moves
 * pixels; the counts are in docs/perf/kasane-lut.md. Both 0 = the pre-LUT
 * reference chain, which is what test_blend_lut.c compares against. */
extern int g_ksn_blend_lut;
extern int g_ksn_blend_lut_alpha;
/* Boundary 3/4 candidate 3c (docs/perf/kasane-group-affine.md): 1 (default)
 * folds a group whose opacity is 255 and whose children are all opaque into one
 * affine map per group -- out = A*src + B*dst + C with A=255, B=C=0 on every
 * channel, applied as a store -- instead of the isolated premultiplied tile
 * chain; 0 keeps the pre-3c chain (tile, premultiply_over, group_over). The fold
 * moves no pixel: every mul8 on the way is exact for these operands. Step 2 of
 * the same workstream extends the map to non-opaque chains, where dropping the
 * intermediate floors does move pixels; that arm is 2 and is NOT the default
 * (measured: 2.2% of pixels, never more than one 5/6/5 level, and no instruction
 * win -- see the document). */
extern int g_ksn_group_affine;
/* Boundary 4a's arithmetic trade, in the scalar blend/pack path: 1 = every RGB
 * mix takes the 255 -> 256 coarse scale (scale256(b) = b + (b>>7), then a plain
 * >>8, and a complementary pair collapses into one rounded mix); 0 = the exact
 * /255 this file has always used, which is also the reference the lane model
 * and the eight-lane kernel are written against. Every *alpha* stays exact in
 * both arms: a 1 -> 0 flip there is not a one-step error but a pixel composited
 * or not, so the coarse arm touches only the three channels that land in
 * pack565. Both paths live in one binary for a same-binary A/B. The default is
 * the measured arm, not the hoped-for one; tools/pie/models/scale256_model.c and
 * docs/perf/kasane-alpha256.md carry the moved-pixel counts and the objdump. */
extern int g_ksn_scale256;
/* Candidate 4a's kernel, the one place where the render path leaves C: 1 = an
 * aligned run of eight pixels of a constant-colour command is handed to
 * `ksn_blend8_pie` (main/ui/kasane/ksn_blend_pie.c) instead of the per-pixel
 * chain; 0 = the chain only. The kernel is exact -- it reproduces blend()'s own
 * expressions, proven over the whole per-channel space and over random multi
 * block runs (test_kernels.py, docs/perf/kasane-blend-pie.md sections 4 and 6) --
 * so both arms must produce byte identical panels. It is default 1 after a
 * same-binary device A/B showed a render gain. PIE is coprocessor 3, owned by
 * the UI task here. The kernel needs
 * a 16-byte aligned destination, so the caller hands over only the 8-aligned
 * window of a run and keeps head and tail on the chain. */
extern int g_ksn_blend_pie;
/* Candidate for binary text masks: aligned eight-pixel blocks are consumed by
 * a zero-copy, per-lane-alpha PIE kernel. Disabled until its own device A/B. */
extern int g_ksn_text_pie;
#ifdef __cplusplus
}
#endif
#endif
