#ifndef KSN_RENDER_H
#define KSN_RENDER_H
#include "ksn_core.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint32_t bands,transferred_bytes; } ksn_render_stats;
/* Bounded production subset: rect, round rect, 1/2 px stroke, two-color
 * horizontal/vertical gradient, command alpha and isolated group opacity.
 * Unsupported commands are rejected before the first transfer. The caller
 * schedules retry/discard; callbacks must not mutate the core or reenter JS. */
ksn_result ksn_render_rects(ksn_core *core,const ksn_display_port *display,ksn_render_stats *stats);
#ifdef __cplusplus
}
#endif
#endif
