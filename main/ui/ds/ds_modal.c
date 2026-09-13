#include "ds_modal.h"

void ds_modal_init(ds_modal *modal,uint32_t focus){
    if(modal)*modal=(ds_modal){.focus=focus,.phase=DS_MODAL_CLOSED};
}
ds_result ds_modal_prepare_open(ds_modal *modal,ds_core *core,ds_tx tx,
                                ds_modal_backdrop backdrop,ds_rgba color,uint32_t initial_focus){
    if(!modal||!core||(backdrop!=DS_MODAL_SOLID&&backdrop!=DS_MODAL_DIM_LIVE))return DS_INVALID;
    if(modal->phase!=DS_MODAL_CLOSED)return DS_BUSY;
    ds_result result=ds_core_check_builder(core,tx,DS_APP,DS_REPLACE);
    if(result!=DS_OK)return result;
    ds_client app=ds_core_client(core,DS_APP);
    if(backdrop==DS_MODAL_SOLID){
        /* SOLID does not secretly retain hidden background commands. */
        ds_capacity usage;result=ds_core_builder_usage(core,tx,&usage);
        if(result!=DS_OK)return result;
        if((color&255)!=255||usage.commands)return DS_INVALID;
        result=app.ops->background(app.ctx,tx,color);
    }else{
        ds_draw scrim={.kind=DS_RECT,.bounds={0,0,240,135},.clip={0,0,240,135},.opacity=255,
                       .data.shape={color,0,0}};
        ds_ref ref;result=app.ops->add(app.ctx,tx,&scrim,&ref);
    }
    if(result!=DS_OK)return result;
    modal->saved_focus=modal->focus;modal->target_focus=initial_focus;
    modal->pending=tx;modal->phase=DS_MODAL_PREPARING;return DS_OK;
}
ds_result ds_modal_prepare_close(ds_modal *modal,const ds_core *core,ds_tx tx){
    if(!modal||!core)return DS_INVALID;
    if(modal->phase!=DS_MODAL_OPEN)return DS_BUSY;
    ds_result result=ds_core_check_builder(core,tx,DS_APP,DS_REPLACE);
    if(result!=DS_OK)return result;
    modal->pending=tx;modal->phase=DS_MODAL_CLOSING;return DS_OK;
}
ds_result ds_modal_resolve(ds_modal *modal,const ds_core *core){
    if(!modal||!core)return DS_INVALID;
    if(!modal->pending.value)return DS_STALE;
    ds_submission outcome=ds_core_poll(core);
    if(outcome.ticket.value!=modal->pending.value||outcome.layer!=DS_APP)return DS_STALE;
    if(outcome.status==DS_SUBMITTED)return DS_BUSY;
    if(outcome.status!=DS_PRESENTED&&outcome.status!=DS_DISCARDED)return DS_STALE;
    bool opening=modal->phase==DS_MODAL_PREPARING;
    if(outcome.status==DS_PRESENTED){
        modal->phase=opening?DS_MODAL_OPEN:DS_MODAL_CLOSED;
        modal->focus=opening?modal->target_focus:modal->saved_focus;
    }else modal->phase=opening?DS_MODAL_CLOSED:DS_MODAL_OPEN;
    modal->pending=(ds_tx){0};return DS_OK;
}
ds_result ds_modal_cancel(ds_modal *modal,ds_core *core){
    if(!modal||!core)return DS_INVALID;
    if(!modal->pending.value)return DS_STALE;
    ds_submission outcome=ds_core_poll(core);
    if(outcome.ticket.value==modal->pending.value){
        if(outcome.status==DS_SUBMITTED){
            ds_result result=ds_core_discard_reason(core,modal->pending,DS_CANCELLED);
            if(result!=DS_OK)return result;
        }
        /* A completed presentation cannot be undone by a late cancel. */
        return ds_modal_resolve(modal,core);
    }
    ds_result result=ds_core_check_builder(core,modal->pending,DS_APP,DS_REPLACE);
    if(result==DS_STALE)return DS_STALE;
    ds_client app=ds_core_client(core,DS_APP);app.ops->abort(app.ctx,modal->pending);
    modal->phase=modal->phase==DS_MODAL_PREPARING?DS_MODAL_CLOSED:DS_MODAL_OPEN;
    modal->pending=(ds_tx){0};return DS_OK;
}
ds_input_scope ds_modal_route(const ds_modal *modal,const ds_core *core,bool host_priority){
    if(host_priority)return DS_INPUT_HOST;
    if(!modal||!core||ds_core_needs_repair(core)||modal->pending.value)return DS_INPUT_BLOCKED;
    return modal->phase==DS_MODAL_OPEN?DS_INPUT_MODAL:DS_INPUT_APP;
}
ds_result ds_modal_focus(ds_modal *modal,const uint32_t *enabled,uint16_t count){
    if(!modal||(count&&!enabled))return DS_INVALID;
    if(modal->pending.value)return DS_BUSY;
    for(unsigned i=0;i<count;i++)if(!enabled[i])return DS_INVALID;
    for(unsigned i=0;i<count;i++)if(enabled[i]==modal->focus)return DS_OK;
    modal->focus=count?enabled[0]:0;return DS_OK;
}
