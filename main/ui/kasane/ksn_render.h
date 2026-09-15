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
/* Bounded production subset: rect, round rect, 1/2 px stroke, two-color
 * horizontal/vertical gradient, font-port TEXT coverage, command alpha and
 * isolated group opacity. The borrowed text port and its immutable resources
 * must remain replayable for pending and committed frames. Coverage scratch is
 * 64 bytes (plus the existing 256-byte group tile), never a text surface.
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
#ifdef __cplusplus
}
#endif
#endif
