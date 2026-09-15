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
