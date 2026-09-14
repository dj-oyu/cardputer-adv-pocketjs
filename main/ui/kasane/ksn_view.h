#ifndef KSN_VIEW_H
#define KSN_VIEW_H
#include "ksn_composition_types.h"
/* App/QuickJS-adapter endpoint. No core, bank, LCD or cache storage escapes.
 * The host binds the endpoint to one layer; all handles are opaque values.
 * Owner task only. No callbacks, allocation, or implicit presentation. */
typedef struct ksn_view ksn_view;
typedef struct {
    uint32_t draw_kinds,cache_kinds;
    bool group_opacity,modal,animation,frosted;
    ksn_capacity capacity;
    uint16_t cache_commands;
    uint8_t cache_templates,cache_instances;
} ksn_view_capabilities;
typedef struct {
    ksn_capacity displayed;
    ksn_cache_stats shared_cache;
    uint32_t native_bytes; /* Core+cache+coordinator+shared IDs; excludes LCD/VM. */
} ksn_view_stats;
#ifdef __cplusplus
extern "C" {
#endif
ksn_view_capabilities ksn_view_features(const ksn_view *);
ksn_view_stats ksn_view_get_stats(const ksn_view *);
ksn_result ksn_view_begin(ksn_view *,ksn_update_mode,ksn_tx *);
ksn_result ksn_view_background(ksn_view *,ksn_tx,ksn_rgba);
ksn_result ksn_view_add(ksn_view *,ksn_tx,const ksn_draw *,ksn_ref *);
ksn_result ksn_view_change(ksn_view *,ksn_tx,ksn_ref,const ksn_change *);
ksn_result ksn_view_group(ksn_view *,ksn_tx,ksn_ref first,uint16_t count,uint8_t opacity);
/* A mutation failure in the current owning builder aborts all its changes.
 * A foreign/stale ticket never cancels another builder. Submit seals only:
 * promote candidate refs after poll says PRESENTED, discard on DISCARDED.
 * Last outcome is retained per layer until that layer submits again. */
ksn_result ksn_view_submit(ksn_view *,ksn_tx);
ksn_result ksn_view_cancel(ksn_view *,ksn_tx);
ksn_submission ksn_view_poll(const ksn_view *);
/* Definitions are copied and live until release or host reset. Create/release
 * only between updates; they are not rolled back by transaction cancellation.
 * Instances detach when omitted from a successfully presented REPLACE. */
ksn_result ksn_view_cache_create(ksn_view *,const ksn_draw *,uint16_t,ksn_template *);
ksn_result ksn_view_cache_release(ksn_view *,ksn_template);
ksn_result ksn_view_instantiate(ksn_view *,ksn_tx,ksn_template,const ksn_placement *,ksn_instance *);
ksn_result ksn_view_place(ksn_view *,ksn_tx,ksn_instance,const ksn_placement *);
ksn_result ksn_view_visible(ksn_view *,ksn_tx,ksn_instance,bool);
/* APP REPLACE only. SOLID first; DIM_LIVE after background content. Add modal
 * content afterwards. Closing REPLACE rebuilds the latest app domain state. */
ksn_result ksn_view_modal_open(ksn_view *,ksn_tx,ksn_modal_backdrop,ksn_rgba,uint32_t focus);
ksn_result ksn_view_modal_close(ksn_view *,ksn_tx);
#ifdef __cplusplus
}
#endif
#endif
