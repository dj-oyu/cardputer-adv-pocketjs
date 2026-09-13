#ifndef DS_RENDER_H
#define DS_RENDER_H
#include "ds_core.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint32_t bands,transferred_bytes; } ds_render_stats;
/* Initial production subset: rect, command alpha and isolated group opacity.
 * Unsupported commands are rejected before the first transfer. The caller
 * schedules retry/discard; callbacks must not mutate the core or reenter JS. */
ds_result ds_render_rects(ds_core *core,const ds_display_port *display,ds_render_stats *stats);
#ifdef __cplusplus
}
#endif
#endif
