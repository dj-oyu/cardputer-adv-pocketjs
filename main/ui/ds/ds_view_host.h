#ifndef DS_VIEW_HOST_H
#define DS_VIEW_HOST_H
#include "ds_view.h"
#include "ds_cache.h"
#include "ds_modal.h"
#include "ds_render.h"
typedef struct ds_view_host ds_view_host;
struct ds_view { ds_view_host *host; ds_layer layer; ds_submission outcome; };
struct ds_view_host {
    ds_core *core;
    ds_cache *cache;
    ds_modal modal;
    ds_view views[2];
    ds_tx builder;
    ds_layer building_layer;
};
#ifdef __cplusplus
extern "C" {
#endif
/* Initializes borrowed core/cache too. One coordinator owns them exclusively;
 * do not mix low-level mutations with view calls. Storage cannot move. Destroy
 * guest endpoints before reset. Existing process-wide ID issuers stay intact. */
void ds_view_host_init(ds_view_host *,ds_core *,ds_cache *,uint32_t initial_focus);
ds_view *ds_view_host_endpoint(ds_view_host *,ds_layer);
/* Call on EVERY return from JS, including exception/yield. Aborts unfinished
 * builders; submitted transactions survive. No JS invocation during cleanup. */
void ds_view_host_end_turn(ds_view_host *);
/* Render then resolve cache/modal before guest/input resumes. IO retains the
 * submission for retry/cancel; partial transfer blocks app input until repair. */
ds_result ds_view_host_present(ds_view_host *,const ds_display_port *,ds_render_stats *);
ds_input_scope ds_view_host_route(const ds_view_host *,bool host_priority);
ds_result ds_view_host_focus(ds_view_host *,const uint32_t *,uint16_t);
#ifdef __cplusplus
}
#endif
#endif
