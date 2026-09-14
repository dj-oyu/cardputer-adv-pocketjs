#include "ds_view_host.h"

static bool valid(const ds_view *v){return v&&v->host&&v->host->core&&v->host->cache;}
static bool owns(const ds_view *v,ds_tx tx){
    return valid(v)&&tx.value&&v->host->builder.value==tx.value&&v->host->building_layer==v->layer;
}
static ds_client client(ds_view *v){return ds_core_client(v->host->core,v->layer);}
static void abort_builder(ds_view_host *h){
    ds_tx tx=h->builder;
    if(!tx.value)return;
    ds_cache_abort(h->cache,tx);
    if(h->modal.pending.value==tx.value)ds_modal_cancel(&h->modal,h->core);
    ds_client c=ds_core_client(h->core,h->building_layer);c.ops->abort(c.ctx,tx);
    h->builder=(ds_tx){0};
}
static ds_result mutation(ds_view *v,ds_tx tx,ds_result result){
    if(result!=DS_OK&&owns(v,tx))abort_builder(v->host);
    return result;
}
static void resolve(ds_view_host *h){
    ds_submission s=ds_core_poll(h->core);
    if(s.status!=DS_PRESENTED&&s.status!=DS_DISCARDED)return;
    ds_cache_resolve(h->cache,h->core,s.ticket,s.status==DS_PRESENTED);
    if(h->modal.pending.value==s.ticket.value)ds_modal_resolve(&h->modal,h->core);
    h->views[s.layer].outcome=s;
}
void ds_view_host_init(ds_view_host *h,ds_core *core,ds_cache *cache,uint32_t focus){
    if(!h||!core||!cache)return;
    *h=(ds_view_host){.core=core,.cache=cache};
    ds_core_init(core);ds_cache_init(cache);ds_modal_init(&h->modal,focus);
    for(unsigned i=0;i<2;i++)h->views[i]=(ds_view){.host=h,.layer=(ds_layer)i,.outcome.layer=(ds_layer)i};
}
ds_view *ds_view_host_endpoint(ds_view_host *h,ds_layer layer){
    return h&&h->core&&h->cache&&(layer==DS_APP||layer==DS_SYSTEM)?&h->views[layer]:NULL;
}
ds_view_capabilities ds_view_features(const ds_view *v){
    if(!valid(v))return (ds_view_capabilities){0};
    return (ds_view_capabilities){.draw_kinds=1u<<DS_RECT,.cache_kinds=1u<<DS_RECT,
        .group_opacity=true,.modal=v->layer==DS_APP,
        .capacity={v->layer==DS_APP?DS_APP_COMMANDS:DS_SYSTEM_COMMANDS,
                   v->layer==DS_APP?DS_APP_TEXT_BYTES:DS_SYSTEM_TEXT_BYTES,0},
        .cache_commands=DS_CACHE_COMMANDS,.cache_templates=DS_CACHE_TEMPLATES,.cache_instances=DS_CACHE_INSTANCES};
}
ds_view_stats ds_view_get_stats(const ds_view *v){
    if(!valid(v))return (ds_view_stats){0};
    return (ds_view_stats){ds_core_active_usage(v->host->core,v->layer),ds_cache_get_stats(v->host->cache),
                          sizeof(ds_core)+sizeof(ds_cache)+sizeof(ds_view_host)+20};
}
ds_result ds_view_begin(ds_view *v,ds_update_mode mode,ds_tx *out){
    if(!valid(v)||!out)return DS_INVALID;
    ds_client c=client(v);ds_result r=c.ops->begin(c.ctx,mode,out);
    if(r==DS_OK){v->host->builder=*out;v->host->building_layer=v->layer;}
    return r;
}
ds_result ds_view_background(ds_view *v,ds_tx tx,ds_rgba color){
    if(!owns(v,tx))return DS_STALE;
    ds_client c=client(v);return mutation(v,tx,c.ops->background(c.ctx,tx,color));
}
ds_result ds_view_add(ds_view *v,ds_tx tx,const ds_draw *d,ds_ref *out){
    if(!owns(v,tx))return DS_STALE;
    if(!d||!out)return mutation(v,tx,DS_INVALID);
    if(d->kind!=DS_RECT)return mutation(v,tx,DS_UNSUPPORTED);
    ds_client c=client(v);return mutation(v,tx,c.ops->add(c.ctx,tx,d,out));
}
ds_result ds_view_change(ds_view *v,ds_tx tx,ds_ref ref,const ds_change *change){
    if(!owns(v,tx))return DS_STALE;
    ds_client c=client(v);return mutation(v,tx,c.ops->change(c.ctx,tx,ref,change));
}
ds_result ds_view_group(ds_view *v,ds_tx tx,ds_ref first,uint16_t count,uint8_t opacity){
    if(!owns(v,tx))return DS_STALE;
    return mutation(v,tx,ds_core_group(v->host->core,v->layer,tx,first,count,opacity));
}
ds_result ds_view_submit(ds_view *v,ds_tx tx){
    if(!owns(v,tx))return DS_STALE;
    ds_client c=client(v);ds_result r=c.ops->end(c.ctx,tx);
    if(r!=DS_OK)return mutation(v,tx,r);
    v->host->builder=(ds_tx){0};v->outcome=ds_core_poll(v->host->core);return DS_OK;
}
ds_result ds_view_cancel(ds_view *v,ds_tx tx){
    if(!valid(v)||!tx.value)return DS_INVALID;
    if(owns(v,tx)){abort_builder(v->host);return DS_OK;}
    ds_submission s=ds_core_poll(v->host->core);
    if(s.ticket.value!=tx.value||s.layer!=v->layer||s.status!=DS_SUBMITTED)return DS_STALE;
    ds_result r=ds_core_discard_reason(v->host->core,tx,DS_CANCELLED);
    if(r==DS_OK)resolve(v->host);
    return r;
}
ds_submission ds_view_poll(const ds_view *v){return valid(v)?v->outcome:(ds_submission){0};}
static bool idle(const ds_view *v){return !v->host->builder.value&&!ds_core_has_submission(v->host->core);}
static bool template_owned(const ds_view *v,ds_template t){
    const ds_cache_impl *c=&v->host->cache->state;
    for(unsigned i=0;i<c->template_count;i++)if(c->templates[i].id==t.value)return c->templates[i].layer==v->layer;
    return false;
}
static bool instance_owned(const ds_view *v,ds_instance t){
    const ds_cache_impl *c=&v->host->cache->state;
    for(unsigned i=0;i<c->instance_count;i++)if(c->instances[i].id==t.value)return c->instances[i].layer==v->layer;
    return false;
}
ds_result ds_view_cache_create(ds_view *v,const ds_draw *d,uint16_t count,ds_template *out){
    if(!valid(v)||!d||!count||!out)return DS_INVALID;
    if(!idle(v))return DS_BUSY;
    if(count>DS_CACHE_COMMANDS)return DS_LIMIT;
    for(unsigned i=0;i<count;i++)if(d[i].kind!=DS_RECT)return DS_UNSUPPORTED;
    return ds_cache_create(v->host->cache,v->layer,d,count,out);
}
ds_result ds_view_cache_release(ds_view *v,ds_template t){
    if(!valid(v))return DS_INVALID;
    if(!idle(v))return DS_BUSY;
    if(!template_owned(v,t))return DS_STALE;
    return ds_cache_release(v->host->cache,t);
}
ds_result ds_view_instantiate(ds_view *v,ds_tx tx,ds_template t,const ds_placement *p,ds_instance *out){
    if(!owns(v,tx))return DS_STALE;
    if(!template_owned(v,t))return mutation(v,tx,DS_STALE);
    return mutation(v,tx,ds_cache_instantiate(v->host->cache,v->host->core,tx,t,p,out));
}
ds_result ds_view_place(ds_view *v,ds_tx tx,ds_instance i,const ds_placement *p){
    if(!owns(v,tx))return DS_STALE;
    if(!instance_owned(v,i))return mutation(v,tx,DS_STALE);
    return mutation(v,tx,ds_cache_place(v->host->cache,v->host->core,tx,i,p));
}
ds_result ds_view_visible(ds_view *v,ds_tx tx,ds_instance i,bool visible){
    if(!owns(v,tx))return DS_STALE;
    if(!instance_owned(v,i))return mutation(v,tx,DS_STALE);
    return mutation(v,tx,ds_cache_set_visible(v->host->cache,v->host->core,tx,i,visible));
}
ds_result ds_view_modal_open(ds_view *v,ds_tx tx,ds_modal_backdrop backdrop,ds_rgba color,uint32_t focus){
    if(!owns(v,tx))return DS_STALE;
    if(v->layer!=DS_APP)return mutation(v,tx,DS_UNSUPPORTED);
    return mutation(v,tx,ds_modal_prepare_open(&v->host->modal,v->host->core,tx,backdrop,color,focus));
}
ds_result ds_view_modal_close(ds_view *v,ds_tx tx){
    if(!owns(v,tx))return DS_STALE;
    if(v->layer!=DS_APP)return mutation(v,tx,DS_UNSUPPORTED);
    return mutation(v,tx,ds_modal_prepare_close(&v->host->modal,v->host->core,tx));
}
void ds_view_host_end_turn(ds_view_host *h){if(h)abort_builder(h);}
ds_result ds_view_host_present(ds_view_host *h,const ds_display_port *port,ds_render_stats *stats){
    if(!h||!h->core||!h->cache)return DS_INVALID;
    ds_result r=ds_render_rects(h->core,port,stats);
    if(r==DS_OK)resolve(h);
    return r;
}
ds_input_scope ds_view_host_route(const ds_view_host *h,bool priority){
    return ds_modal_route(h?&h->modal:NULL,h?h->core:NULL,priority);
}
ds_result ds_view_host_focus(ds_view_host *h,const uint32_t *keys,uint16_t count){
    return h?ds_modal_focus(&h->modal,keys,count):DS_INVALID;
}
