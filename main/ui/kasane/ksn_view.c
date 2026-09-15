#include "ksn_view_host.h"

static bool valid(const ksn_view *v){return v&&v->host&&v->host->core;}
static bool owns(const ksn_view *v,ksn_tx tx){
    return valid(v)&&tx.value&&v->host->builder.value==tx.value&&v->host->building_layer==v->layer;
}
static ksn_client client(ksn_view *v){return ksn_core_client(v->host->core,v->layer);}
static void abort_builder(ksn_view_host *h){
    ksn_tx tx=h->builder;
    if(!tx.value)return;
    ksn_cache_abort(h->cache,tx);
    if(h->modal.pending.value==tx.value)ksn_modal_cancel(&h->modal,h->core);
    ksn_client c=ksn_core_client(h->core,h->building_layer);c.ops->abort(c.ctx,tx);
    h->builder=(ksn_tx){0};
}
static ksn_result mutation(ksn_view *v,ksn_tx tx,ksn_result result){
    if(result!=KSN_OK&&owns(v,tx))abort_builder(v->host);
    return result;
}
static void resolve(ksn_view_host *h){
    ksn_submission s=ksn_core_poll(h->core);
    if(s.status!=KSN_PRESENTED&&s.status!=KSN_DISCARDED)return;
    ksn_cache_resolve(h->cache,h->core,s.ticket,s.status==KSN_PRESENTED);
    if(h->modal.pending.value==s.ticket.value)ksn_modal_resolve(&h->modal,h->core);
    h->views[s.layer].outcome=s;
}
void ksn_view_host_init(ksn_view_host *h,ksn_core *core,ksn_cache *cache,uint32_t focus){
    if(!h||!core)return;
    *h=(ksn_view_host){.core=core,.cache=cache};
    ksn_core_init(core);ksn_cache_init(cache);ksn_modal_init(&h->modal,focus);
    for(unsigned i=0;i<2;i++)h->views[i]=(ksn_view){.host=h,.layer=(ksn_layer)i,.outcome.layer=(ksn_layer)i};
}
ksn_view *ksn_view_host_endpoint(ksn_view_host *h,ksn_layer layer){
    return h&&h->core&&(layer==KSN_APP||layer==KSN_SYSTEM)?&h->views[layer]:NULL;
}
ksn_view_capabilities ksn_view_features(const ksn_view *v){
    if(!valid(v))return (ksn_view_capabilities){0};
    return (ksn_view_capabilities){.draw_kinds=(1u<<KSN_RECT)|(1u<<KSN_ROUND_RECT)|
        (1u<<KSN_STROKE)|(1u<<KSN_GRADIENT),
        .cache_kinds=(1u<<KSN_RECT)|(1u<<KSN_ROUND_RECT)|(1u<<KSN_STROKE),
        .group_opacity=true,.modal=v->layer==KSN_APP,
        .capacity={v->layer==KSN_APP?KSN_APP_COMMANDS:KSN_SYSTEM_COMMANDS,
                   v->layer==KSN_APP?KSN_APP_TEXT_BYTES:KSN_SYSTEM_TEXT_BYTES,0},
        .cache_commands=KSN_CACHE_COMMANDS,.cache_templates=KSN_CACHE_TEMPLATES,.cache_instances=KSN_CACHE_INSTANCES};
}
ksn_view_stats ksn_view_get_stats(const ksn_view *v){
    if(!valid(v))return (ksn_view_stats){0};
    ksn_cache_stats cache=ksn_cache_get_stats(v->host->cache);
    return (ksn_view_stats){ksn_core_active_usage(v->host->core,v->layer),cache,
                          sizeof(ksn_core)+sizeof(ksn_view_host)+20+
                          (v->host->cache?KSN_CACHE_RESERVED_BYTES:0)};
}
ksn_result ksn_view_begin(ksn_view *v,ksn_update_mode mode,ksn_tx *out){
    if(!valid(v)||!out)return KSN_INVALID;
    ksn_client c=client(v);ksn_result r=c.ops->begin(c.ctx,mode,out);
    if(r==KSN_OK){v->host->builder=*out;v->host->building_layer=v->layer;}
    return r;
}
ksn_result ksn_view_background(ksn_view *v,ksn_tx tx,ksn_rgba color){
    if(!owns(v,tx))return KSN_STALE;
    ksn_client c=client(v);return mutation(v,tx,c.ops->background(c.ctx,tx,color));
}
ksn_result ksn_view_add(ksn_view *v,ksn_tx tx,const ksn_draw *d,ksn_ref *out){
    if(!owns(v,tx))return KSN_STALE;
    if(!d||!out)return mutation(v,tx,KSN_INVALID);
    if(d->kind<KSN_RECT||d->kind>KSN_GRADIENT)return mutation(v,tx,KSN_UNSUPPORTED);
    ksn_client c=client(v);return mutation(v,tx,c.ops->add(c.ctx,tx,d,out));
}
ksn_result ksn_view_change(ksn_view *v,ksn_tx tx,ksn_ref ref,const ksn_change *change){
    if(!owns(v,tx))return KSN_STALE;
    ksn_client c=client(v);return mutation(v,tx,c.ops->change(c.ctx,tx,ref,change));
}
ksn_result ksn_view_group(ksn_view *v,ksn_tx tx,ksn_ref first,uint16_t count,uint8_t opacity){
    if(!owns(v,tx))return KSN_STALE;
    return mutation(v,tx,ksn_core_group(v->host->core,v->layer,tx,first,count,opacity));
}
ksn_result ksn_view_submit(ksn_view *v,ksn_tx tx){
    if(!owns(v,tx))return KSN_STALE;
    ksn_client c=client(v);ksn_result r=c.ops->end(c.ctx,tx);
    if(r!=KSN_OK)return mutation(v,tx,r);
    v->host->builder=(ksn_tx){0};v->outcome=ksn_core_poll(v->host->core);return KSN_OK;
}
ksn_result ksn_view_cancel(ksn_view *v,ksn_tx tx){
    if(!valid(v)||!tx.value)return KSN_INVALID;
    if(owns(v,tx)){abort_builder(v->host);return KSN_OK;}
    ksn_submission s=ksn_core_poll(v->host->core);
    if(s.ticket.value!=tx.value||s.layer!=v->layer||s.status!=KSN_SUBMITTED)return KSN_STALE;
    ksn_result r=ksn_core_discard_reason(v->host->core,tx,KSN_CANCELLED);
    if(r==KSN_OK)resolve(v->host);
    return r;
}
ksn_submission ksn_view_poll(const ksn_view *v){return valid(v)?v->outcome:(ksn_submission){0};}
static bool idle(const ksn_view *v){return !v->host->builder.value&&!ksn_core_has_submission(v->host->core);}
static bool template_owned(const ksn_view *v,ksn_template t){
    if(!v->host->cache)return false;
    const ksn_cache_impl *c=&v->host->cache->state;
    for(unsigned i=0;i<c->template_count;i++)if(c->templates[i].id==t.value)return c->templates[i].layer==v->layer;
    return false;
}
static bool instance_owned(const ksn_view *v,ksn_instance t){
    if(!v->host->cache)return false;
    const ksn_cache_impl *c=&v->host->cache->state;
    for(unsigned i=0;i<c->instance_count;i++)if(c->instances[i].id==t.value)return c->instances[i].layer==v->layer;
    return false;
}
ksn_result ksn_view_cache_create(ksn_view *v,const ksn_draw *d,uint16_t count,ksn_template *out){
    if(!valid(v)||!d||!count||!out)return KSN_INVALID;
    if(!idle(v))return KSN_BUSY;
    if(!v->host->cache)return KSN_UNSUPPORTED;
    if(count>KSN_CACHE_COMMANDS)return KSN_LIMIT;
    for(unsigned i=0;i<count;i++)
        if(d[i].kind!=KSN_RECT&&d[i].kind!=KSN_ROUND_RECT&&d[i].kind!=KSN_STROKE)
            return KSN_UNSUPPORTED;
    return ksn_cache_create(v->host->cache,v->layer,d,count,out);
}
ksn_result ksn_view_cache_release(ksn_view *v,ksn_template t){
    if(!valid(v))return KSN_INVALID;
    if(!idle(v))return KSN_BUSY;
    if(!template_owned(v,t))return KSN_STALE;
    return ksn_cache_release(v->host->cache,t);
}
ksn_result ksn_view_instantiate(ksn_view *v,ksn_tx tx,ksn_template t,const ksn_placement *p,ksn_instance *out){
    if(!owns(v,tx))return KSN_STALE;
    if(!template_owned(v,t))return mutation(v,tx,KSN_STALE);
    return mutation(v,tx,ksn_cache_instantiate(v->host->cache,v->host->core,tx,t,p,out));
}
ksn_result ksn_view_place(ksn_view *v,ksn_tx tx,ksn_instance i,const ksn_placement *p){
    if(!owns(v,tx))return KSN_STALE;
    if(!instance_owned(v,i))return mutation(v,tx,KSN_STALE);
    return mutation(v,tx,ksn_cache_place(v->host->cache,v->host->core,tx,i,p));
}
ksn_result ksn_view_visible(ksn_view *v,ksn_tx tx,ksn_instance i,bool visible){
    if(!owns(v,tx))return KSN_STALE;
    if(!instance_owned(v,i))return mutation(v,tx,KSN_STALE);
    return mutation(v,tx,ksn_cache_set_visible(v->host->cache,v->host->core,tx,i,visible));
}
ksn_result ksn_view_modal_open(ksn_view *v,ksn_tx tx,ksn_modal_backdrop backdrop,ksn_rgba color,uint32_t focus){
    if(!owns(v,tx))return KSN_STALE;
    if(v->layer!=KSN_APP)return mutation(v,tx,KSN_UNSUPPORTED);
    return mutation(v,tx,ksn_modal_prepare_open(&v->host->modal,v->host->core,tx,backdrop,color,focus));
}
ksn_result ksn_view_modal_close(ksn_view *v,ksn_tx tx){
    if(!owns(v,tx))return KSN_STALE;
    if(v->layer!=KSN_APP)return mutation(v,tx,KSN_UNSUPPORTED);
    return mutation(v,tx,ksn_modal_prepare_close(&v->host->modal,v->host->core,tx));
}
void ksn_view_host_end_turn(ksn_view_host *h){if(h)abort_builder(h);}
ksn_result ksn_view_host_present(ksn_view_host *h,const ksn_display_port *port,ksn_render_stats *stats){
    if(!h||!h->core||!stats)return KSN_INVALID;
    *stats=(ksn_render_stats){0};
    bool submitted=ksn_core_has_submission(h->core);
    if(!submitted&&!ksn_core_needs_repair(h->core))return KSN_OK;
    ksn_result r=ksn_render_rects(h->core,port,stats);
    if(r==KSN_OK&&submitted)resolve(h);
    return r;
}
ksn_result ksn_view_host_attach_cache(ksn_view_host *h,ksn_cache *cache){
    if(!h||!h->core||!cache||!cache->state.commands||!cache->state.text)return KSN_INVALID;
    if(h->cache||h->builder.value||ksn_core_has_submission(h->core))return KSN_BUSY;
    /* The host may have prepared the first template in these blocks. Attach
     * without reset, and leave both layer outcomes and committed core intact. */
    h->cache=cache;return KSN_OK;
}
void ksn_view_host_invalidate(ksn_view_host *h){if(h)ksn_core_invalidate(h->core);}
bool ksn_view_host_needs_present(const ksn_view_host *h){
    return h&&h->core&&(ksn_core_has_submission(h->core)||ksn_core_needs_repair(h->core));
}
ksn_input_scope ksn_view_host_route(const ksn_view_host *h,bool priority){
    return ksn_modal_route(h?&h->modal:NULL,h?h->core:NULL,priority);
}
ksn_result ksn_view_host_focus(ksn_view_host *h,const uint32_t *keys,uint16_t count){
    return h?ksn_modal_focus(&h->modal,keys,count):KSN_INVALID;
}
