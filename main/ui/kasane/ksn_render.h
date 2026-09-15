#ifndef KSN_RENDER_H
#define KSN_RENDER_H
#include "ksn_core.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint32_t bands,transferred_bytes; } ksn_render_stats;
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
#ifdef __cplusplus
}
#endif
#endif
