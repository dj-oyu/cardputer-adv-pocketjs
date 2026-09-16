#include "ksn_modal.h"

void ksn_modal_init(ksn_modal *modal,uint32_t focus){
    if(modal)*modal=(ksn_modal){.focus=focus,.phase=KSN_MODAL_CLOSED};
}
ksn_result ksn_modal_prepare_open(ksn_modal *modal,ksn_core *core,ksn_tx tx,
                                ksn_modal_backdrop backdrop,ksn_rgba color,uint32_t initial_focus){
    if(!modal||!core||(backdrop!=KSN_MODAL_SOLID&&backdrop!=KSN_MODAL_DIM_LIVE))return KSN_INVALID;
    if(modal->phase!=KSN_MODAL_CLOSED)return KSN_BUSY;
    ksn_result result=ksn_core_check_builder(core,tx,KSN_APP,KSN_REPLACE);
    if(result!=KSN_OK)return result;
    ksn_client app=ksn_core_client(core,KSN_APP);
    if(backdrop==KSN_MODAL_SOLID){
        /* SOLID does not secretly retain hidden background commands. */
        ksn_capacity usage;result=ksn_core_builder_usage(core,tx,&usage);
        if(result!=KSN_OK)return result;
        if((color&255)!=255||usage.commands)return KSN_INVALID;
        result=app.ops->background(app.ctx,tx,color);
    }else{
        ksn_draw scrim={.kind=KSN_RECT,.bounds={0,0,240,135},.clip={0,0,240,135},.opacity=255,
                       .data.shape={color,0,0}};
        ksn_ref ref;result=app.ops->add(app.ctx,tx,&scrim,&ref);
    }
    if(result!=KSN_OK)return result;
    modal->saved_focus=modal->focus;modal->target_focus=initial_focus;
    modal->pending=tx;modal->phase=KSN_MODAL_PREPARING;return KSN_OK;
}
ksn_result ksn_modal_prepare_close(ksn_modal *modal,const ksn_core *core,ksn_tx tx){
    if(!modal||!core)return KSN_INVALID;
    if(modal->phase!=KSN_MODAL_OPEN)return KSN_BUSY;
    ksn_result result=ksn_core_check_builder(core,tx,KSN_APP,KSN_REPLACE);
    if(result!=KSN_OK)return result;
    modal->pending=tx;modal->phase=KSN_MODAL_CLOSING;return KSN_OK;
}
ksn_result ksn_modal_resolve(ksn_modal *modal,const ksn_core *core){
    if(!modal||!core)return KSN_INVALID;
    if(!modal->pending.value)return KSN_STALE;
    ksn_submission outcome=ksn_core_poll(core);
    if(outcome.ticket.value!=modal->pending.value||outcome.layer!=KSN_APP)return KSN_STALE;
    if(outcome.status==KSN_SUBMITTED)return KSN_BUSY;
    if(outcome.status!=KSN_PRESENTED&&outcome.status!=KSN_DISCARDED)return KSN_STALE;
    bool opening=modal->phase==KSN_MODAL_PREPARING;
    if(outcome.status==KSN_PRESENTED){
        modal->phase=opening?KSN_MODAL_OPEN:KSN_MODAL_CLOSED;
        modal->focus=opening?modal->target_focus:modal->saved_focus;
    }else modal->phase=opening?KSN_MODAL_CLOSED:KSN_MODAL_OPEN;
    modal->pending=(ksn_tx){0};return KSN_OK;
}
ksn_result ksn_modal_cancel(ksn_modal *modal,ksn_core *core){
    if(!modal||!core)return KSN_INVALID;
    if(!modal->pending.value)return KSN_STALE;
    ksn_submission outcome=ksn_core_poll(core);
    if(outcome.ticket.value==modal->pending.value){
        if(outcome.status==KSN_SUBMITTED){
            ksn_result result=ksn_core_discard_reason(core,modal->pending,KSN_CANCELLED);
            if(result!=KSN_OK)return result;
        }
        /* A completed presentation cannot be undone by a late cancel. */
        return ksn_modal_resolve(modal,core);
    }
    ksn_result result=ksn_core_check_builder(core,modal->pending,KSN_APP,KSN_REPLACE);
    if(result==KSN_STALE)return KSN_STALE;
    ksn_client app=ksn_core_client(core,KSN_APP);app.ops->abort(app.ctx,modal->pending);
    modal->phase=modal->phase==KSN_MODAL_PREPARING?KSN_MODAL_CLOSED:KSN_MODAL_OPEN;
    modal->pending=(ksn_tx){0};return KSN_OK;
}
ksn_input_scope ksn_modal_route(const ksn_modal *modal,const ksn_core *core,bool host_priority){
    if(host_priority)return KSN_INPUT_HOST;
    if(!modal||!core||ksn_core_needs_repair(core)||modal->pending.value)return KSN_INPUT_BLOCKED;
    return modal->phase==KSN_MODAL_OPEN?KSN_INPUT_MODAL:KSN_INPUT_APP;
}
ksn_result ksn_modal_focus(ksn_modal *modal,const uint32_t *enabled,uint16_t count){
    if(!modal||(count&&!enabled))return KSN_INVALID;
    if(modal->pending.value)return KSN_BUSY;
    for(unsigned i=0;i<count;i++)if(!enabled[i])return KSN_INVALID;
    for(unsigned i=0;i<count;i++)if(enabled[i]==modal->focus)return KSN_OK;
    modal->focus=count?enabled[0]:0;return KSN_OK;
}
