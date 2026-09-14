#ifndef KSN_VIEW_HOST_H
#define KSN_VIEW_HOST_H
#include "ksn_view.h"
#include "ksn_cache.h"
#include "ksn_modal.h"
#include "ksn_render.h"
typedef struct ksn_view_host ksn_view_host;
struct ksn_view { ksn_view_host *host; ksn_layer layer; ksn_submission outcome; };
struct ksn_view_host {
    ksn_core *core;
    ksn_cache *cache;
    ksn_modal modal;
    ksn_view views[2];
    ksn_tx builder;
    ksn_layer building_layer;
};
#ifdef __cplusplus
extern "C" {
#endif
/* Initializes borrowed core/cache too. One coordinator owns them exclusively;
 * do not mix low-level mutations with view calls. Storage cannot move. Destroy
 * guest endpoints before reset. Existing process-wide ID issuers stay intact. */
void ksn_view_host_init(ksn_view_host *,ksn_core *,ksn_cache *,uint32_t initial_focus);
ksn_view *ksn_view_host_endpoint(ksn_view_host *,ksn_layer);
/* Call on EVERY return from JS, including exception/yield. Aborts unfinished
 * builders; submitted transactions survive. No JS invocation during cleanup. */
void ksn_view_host_end_turn(ksn_view_host *);
/* Render then resolve cache/modal before guest/input resumes. IO retains the
 * submission for retry/cancel; partial transfer blocks app input until repair. */
ksn_result ksn_view_host_present(ksn_view_host *,const ksn_display_port *,ksn_render_stats *);
ksn_input_scope ksn_view_host_route(const ksn_view_host *,bool host_priority);
ksn_result ksn_view_host_focus(ksn_view_host *,const uint32_t *,uint16_t);
#ifdef __cplusplus
}
#endif
#endif
