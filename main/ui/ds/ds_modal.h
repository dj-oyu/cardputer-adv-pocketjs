#ifndef DS_MODAL_H
#define DS_MODAL_H
#include "ds_core.h"

typedef struct {
    ds_tx pending;
    uint32_t focus,saved_focus,target_focus;
    ds_modal_phase phase;
} ds_modal;

#ifdef __cplusplus
extern "C" {
#endif
/* Host owner only. Keys are stable numeric application focus keys; 0 is none.
 * Calls and resolve run at the same boundary as cache bookkeeping, before JS
 * resumes. No callbacks, retained input, timer or allocation. Route each input
 * once on arrival; never re-route the input that requested a close/cancel. */
void ds_modal_init(ds_modal *modal,uint32_t focus);
/* APP REPLACE: SOLID must precede all content; DIM_LIVE follows normal content
 * and appends one scrim. Add modal content after this call, then end the tx. */
ds_result ds_modal_prepare_open(ds_modal *modal,ds_core *core,ds_tx tx,
                                ds_modal_backdrop backdrop,ds_rgba color,uint32_t initial_focus);
/* APP REPLACE containing the restored current domain state. */
ds_result ds_modal_prepare_close(ds_modal *modal,const ds_core *core,ds_tx tx);
/* After core end -> render/discard. Wrong/old results cannot change scope. */
ds_result ds_modal_resolve(ds_modal *modal,const ds_core *core);
/* Cancel builder/submission, restoring the prior modal state. Pair with cache
 * abort/resolve using the pending ticket if that transaction used a cache.
 * Partial LCD failure blocks APP input until a successful repair submission. */
ds_result ds_modal_cancel(ds_modal *modal,ds_core *core);
ds_input_scope ds_modal_route(const ds_modal *modal,const ds_core *core,bool host_priority);
/* Reconcile only against the currently active scope's enabled focus keys. */
ds_result ds_modal_focus(ds_modal *modal,const uint32_t *enabled,uint16_t count);
#ifdef __cplusplus
}
#endif
#endif
