#ifndef KSN_RENDER_H
#define KSN_RENDER_H
#include "ksn_core.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint32_t bands,transferred_bytes; } ksn_render_stats;
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
/* Bounded production subset: rect, round rect, 1/2 px stroke, two-color
 * horizontal/vertical gradient, font-port TEXT, source-span IMAGE, alpha and
 * isolated group opacity. The borrowed text port and its immutable resources
 * must remain replayable for pending and committed frames. One shared 96-byte
 * span scratch plus group tile/provenance (264) and provider row (up to 128)
 * fit the 512-byte pixel scratch budget. No component surface.
 * Unsupported commands are rejected before the first transfer. The caller
 * schedules retry/discard; callbacks may request invalidation, but must not
 * otherwise mutate the core or reenter JS/presentation. prepare_frame consumes
 * preexisting invalidation; later invalidation survives the acknowledgement. */
ksn_result ksn_render_rects(ksn_core *core,const ksn_display_port *display,ksn_render_stats *stats);
/* TEMPORARY A/B switch for that renderer's coverage solver: 1 = solve a row's
 * covered x set as runs once per row per command, 0 = ask the per-pixel
 * predicate for every pixel. Both arms live in one binary and produce the same
 * pixels (test_coverage_spans.c compares them exhaustively and frame by frame). */
extern int g_ksn_row_coverage;
#ifdef __cplusplus
}
#endif
#endif
