#ifndef DS_VIEW_H
#define DS_VIEW_H
#include "ds_composition_types.h"
/* App/QuickJS-adapter endpoint. No core, bank, LCD or cache storage escapes.
 * The host binds the endpoint to one layer; all handles are opaque values.
 * Owner task only. No callbacks, allocation, or implicit presentation. */
typedef struct ds_view ds_view;
typedef struct {
    uint32_t draw_kinds,cache_kinds;
    bool group_opacity,modal,animation,frosted;
    ds_capacity capacity;
    uint16_t cache_commands;
    uint8_t cache_templates,cache_instances;
} ds_view_capabilities;
typedef struct {
    ds_capacity displayed;
    ds_cache_stats shared_cache;
    uint32_t native_bytes; /* Core+cache+coordinator+shared IDs; excludes LCD/VM. */
} ds_view_stats;
#ifdef __cplusplus
extern "C" {
#endif
ds_view_capabilities ds_view_features(const ds_view *);
ds_view_stats ds_view_get_stats(const ds_view *);
ds_result ds_view_begin(ds_view *,ds_update_mode,ds_tx *);
ds_result ds_view_background(ds_view *,ds_tx,ds_rgba);
ds_result ds_view_add(ds_view *,ds_tx,const ds_draw *,ds_ref *);
ds_result ds_view_change(ds_view *,ds_tx,ds_ref,const ds_change *);
ds_result ds_view_group(ds_view *,ds_tx,ds_ref first,uint16_t count,uint8_t opacity);
/* A mutation failure in the current owning builder aborts all its changes.
 * A foreign/stale ticket never cancels another builder. Submit seals only:
 * promote candidate refs after poll says PRESENTED, discard on DISCARDED.
 * Last outcome is retained per layer until that layer submits again. */
ds_result ds_view_submit(ds_view *,ds_tx);
ds_result ds_view_cancel(ds_view *,ds_tx);
ds_submission ds_view_poll(const ds_view *);
/* Definitions are copied and live until release or host reset. Create/release
 * only between updates; they are not rolled back by transaction cancellation.
 * Instances detach when omitted from a successfully presented REPLACE. */
ds_result ds_view_cache_create(ds_view *,const ds_draw *,uint16_t,ds_template *);
ds_result ds_view_cache_release(ds_view *,ds_template);
ds_result ds_view_instantiate(ds_view *,ds_tx,ds_template,const ds_placement *,ds_instance *);
ds_result ds_view_place(ds_view *,ds_tx,ds_instance,const ds_placement *);
ds_result ds_view_visible(ds_view *,ds_tx,ds_instance,bool);
/* APP REPLACE only. SOLID first; DIM_LIVE after background content. Add modal
 * content afterwards. Closing REPLACE rebuilds the latest app domain state. */
ds_result ds_view_modal_open(ds_view *,ds_tx,ds_modal_backdrop,ds_rgba,uint32_t focus);
ds_result ds_view_modal_close(ds_view *,ds_tx);
#ifdef __cplusplus
}
#endif
#endif
