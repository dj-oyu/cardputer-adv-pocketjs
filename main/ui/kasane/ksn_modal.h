#ifndef KSN_MODAL_H
#define KSN_MODAL_H
#include "ksn_core.h"

typedef struct {
    ksn_tx pending;
    uint32_t focus,saved_focus,target_focus;
    ksn_modal_phase phase;
} ksn_modal;

#ifdef __cplusplus
extern "C" {
#endif
/* Host owner only. Keys are stable numeric application focus keys; 0 is none.
 * Calls and resolve run at the same boundary as cache bookkeeping, before JS
 * resumes. No callbacks, retained input, timer or allocation. Route each input
 * once on arrival; never re-route the input that requested a close/cancel. */
void ksn_modal_init(ksn_modal *modal,uint32_t focus);
/* APP REPLACE: SOLID must precede all content; DIM_LIVE follows normal content
 * and appends one scrim. Add modal content after this call, then end the tx. */
ksn_result ksn_modal_prepare_open(ksn_modal *modal,ksn_core *core,ksn_tx tx,
                                ksn_modal_backdrop backdrop,ksn_rgba color,uint32_t initial_focus);
/* APP REPLACE containing the restored current domain state. */
ksn_result ksn_modal_prepare_close(ksn_modal *modal,const ksn_core *core,ksn_tx tx);
/* After core end -> render/discard. Wrong/old results cannot change scope. */
ksn_result ksn_modal_resolve(ksn_modal *modal,const ksn_core *core);
/* Cancel builder/submission, restoring the prior modal state. Pair with cache
 * abort/resolve using the pending ticket if that transaction used a cache.
 * Partial LCD failure blocks APP input until a successful repair submission. */
ksn_result ksn_modal_cancel(ksn_modal *modal,ksn_core *core);
ksn_input_scope ksn_modal_route(const ksn_modal *modal,const ksn_core *core,bool host_priority);
/* Reconcile only against the currently active scope's enabled focus keys. */
ksn_result ksn_modal_focus(ksn_modal *modal,const uint32_t *enabled,uint16_t count);
#ifdef __cplusplus
}
#endif
#endif
