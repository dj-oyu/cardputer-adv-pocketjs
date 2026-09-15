#ifndef KSN_RENDER_H
#define KSN_RENDER_H
#include "ksn_core.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint32_t bands,transferred_bytes; } ksn_render_stats;
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
#ifdef __cplusplus
}
#endif
#endif
