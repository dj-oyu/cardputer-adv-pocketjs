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
    bool presenting;
};
#ifdef __cplusplus
extern "C" {
#endif
/* Resets the already bound borrowed core/cache. Cache may be NULL; a supplied cache
 * must already be bound, and reset preserves its block addresses.
 * One coordinator owns them exclusively;
 * do not mix low-level mutations with view calls. Storage cannot move. Destroy
 * guest endpoints before reset. Existing process-wide ID issuers stay intact. */
void ksn_view_host_init(ksn_view_host *,ksn_core *,ksn_cache *,uint32_t initial_focus);
ksn_view *ksn_view_host_endpoint(ksn_view_host *,ksn_layer);
/* Enable an optional bound cache only between guest updates. Does not reset
 * it or allocate; failure leaves the coordinator and supplied cache unchanged. */
ksn_result ksn_view_host_attach_cache(ksn_view_host *,ksn_cache *);
/* Owner-only APP teardown. Cancels APP work and drops its two banks, images,
 * cache entries and modal/focus. SYSTEM builders/submissions/refs survive.
 * Forces full repaint before input resumes. No allocation or guest callback.
 * Stop guest access first: this operation does not revoke endpoint pointers.
 * Calling from a display/provider callback returns BUSY without mutation. */
ksn_result ksn_view_host_reset_app(ksn_view_host *);
/* Call on EVERY return from JS, including exception/yield. Aborts unfinished
 * builders; submitted transactions survive. No JS invocation during cleanup. */
void ksn_view_host_end_turn(ksn_view_host *);
/* Render then resolve cache/modal before guest/input resumes. IO retains the
 * submission for retry/cancel; partial transfer blocks app input until repair.
 * Without a submission, redraw the committed bank when repair is pending. */
ksn_result ksn_view_host_present(ksn_view_host *,const ksn_display_port *,ksn_render_stats *);
void ksn_view_host_invalidate(ksn_view_host *);
bool ksn_view_host_needs_present(const ksn_view_host *);
ksn_input_scope ksn_view_host_route(const ksn_view_host *,bool host_priority);
ksn_result ksn_view_host_focus(ksn_view_host *,const uint32_t *,uint16_t);
#ifdef __cplusplus
}
#endif
#endif
