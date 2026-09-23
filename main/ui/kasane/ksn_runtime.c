#include "ksn_runtime.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    ksn_core core;
    ksn_view_host host;
    ksn_cache *cache;
    ksn_core_animation_block *animations;
    ksn_app_lease app;
    /* The APP owner's control state (pocket_kasane.c's kasane_state): inside
     * this block when the block was created for the APP, else its own calloc. */
    void *tail;
    uint32_t tail_capacity;
    bool tail_separate;
    bool system_owned,app_active,hidden,reduce_motion;
} runtime_storage;
/* One allocation for the control state, both command banks and both text
 * banks (and the APP tail). They used to be five callocs made while the guest
 * was evaluating, and each cut a hole in the largest free block
 * (docs/kasane/kasane-guest-memory-reduce.md). */
typedef struct {
    runtime_storage control;
    ksn_core_command_block commands[2];
    ksn_core_text_block text[2];
} runtime_block;
static runtime_storage *runtime;
/* Process-lifetime identities prevent leases reviving after heap address reuse. */
static uint32_t last_lease;
_Static_assert(sizeof(runtime_storage)<=3072,"runtime control allocation budget");
#define BASE_BYTES ((sizeof(runtime_block)+KSN_RUNTIME_TAIL_ALIGN-1)&~(size_t)(KSN_RUNTIME_TAIL_ALIGN-1))
_Static_assert(BASE_BYTES<=KSN_RUNTIME_BASE_BUDGET,"runtime block budget");
_Static_assert(_Alignof(runtime_block)<=KSN_RUNTIME_TAIL_ALIGN,"tail offset alignment");

static void free_cache(ksn_cache *cache){
    if(cache){free(cache->state.commands);free(cache->state.text);free(cache);}
}
static void release_tail(void){
    if(runtime->tail_separate)free(runtime->tail);
    runtime->tail=NULL;runtime->tail_separate=false;
}
static void destroy(void){
    free(runtime->animations);
    free_cache(runtime->cache);
    release_tail();
    free(runtime);runtime=NULL;
}
/* tail: bytes reserved after the block for the APP owner, 0 for SYSTEM. */
static ksn_result ensure(uint32_t tail){
    if(runtime)return KSN_OK;
    runtime_block *b=calloc(1,BASE_BYTES+tail);
    if(!b)return KSN_OOM;
    runtime_storage *r=&b->control;
    ksn_core_bind(&r->core,&b->commands[0],&b->commands[1],&b->text[0],&b->text[1]);
    ksn_view_host_init(&r->host,&r->core,NULL,0);
    r->tail_capacity=tail;
    runtime=r;return KSN_OK;
}
ksn_view *ksn_runtime_app_view(ksn_app_lease lease){
    return runtime&&lease.value&&lease.value==runtime->app.value?
           ksn_view_host_endpoint(&runtime->host,KSN_APP):NULL;
}
ksn_view *ksn_runtime_app_system_view(ksn_app_lease lease){
    return ksn_runtime_app_view(lease)&&!runtime->system_owned?
        ksn_view_host_endpoint(&runtime->host,KSN_SYSTEM):NULL;
}
ksn_result ksn_runtime_app_attach(ksn_app_lease *out){return ksn_runtime_app_attach_tail(out,0,NULL);}
ksn_result ksn_runtime_app_attach_tail(ksn_app_lease *out,uint32_t bytes,void **tail){
    if(!out||(bytes&&!tail)||bytes>KSN_RUNTIME_TAIL_BUDGET)return KSN_INVALID;
    if(runtime&&(runtime->app.value||runtime->host.presenting))return KSN_BUSY;
    if(last_lease==UINT32_MAX)return KSN_LIMIT;
    bool created=!runtime;
    ksn_result r=ensure(bytes);if(r!=KSN_OK)return r;
    if(bytes){
        void *p;
        if(runtime->tail_capacity>=bytes){p=(char *)runtime+BASE_BYTES;memset(p,0,bytes);}
        else if(!(p=calloc(1,bytes))){if(created)destroy();return KSN_OOM;}
        else runtime->tail_separate=true;
        runtime->tail=p;*tail=p;
    }
    runtime->app=(ksn_app_lease){++last_lease};*out=runtime->app;return KSN_OK;
}
ksn_result ksn_runtime_app_detach(ksn_app_lease lease){
    if(!ksn_runtime_app_view(lease))return KSN_STALE;
    ksn_result r=ksn_view_host_reset_app(&runtime->host);if(r!=KSN_OK)return r;
    runtime->app=(ksn_app_lease){0};runtime->app_active=false;
    if(!runtime->system_owned)destroy();
    else release_tail();
    return KSN_OK;
}
void ksn_runtime_app_end_turn(ksn_app_lease lease){
    if(ksn_runtime_app_view(lease)&&runtime->host.building_layer==KSN_APP)
        ksn_view_host_end_turn(&runtime->host);
}
void ksn_runtime_app_activate(ksn_app_lease lease){
    if(ksn_runtime_app_view(lease))runtime->app_active=true;
}
ksn_result ksn_runtime_system_acquire(ksn_view **out){
    if(!out)return KSN_INVALID;
    ksn_result r=ensure(0);if(r!=KSN_OK)return r;
    runtime->system_owned=true;*out=ksn_view_host_endpoint(&runtime->host,KSN_SYSTEM);
    return KSN_OK;
}
ksn_result ksn_runtime_shutdown(void){
    if(!runtime)return KSN_OK;
    if(runtime->app.value||runtime->host.presenting||runtime->host.builder.value||
       ksn_core_has_submission(&runtime->core))return KSN_BUSY;
    destroy();return KSN_OK;
}
ksn_result ksn_runtime_cache_create(ksn_view *v,const ksn_draw *draws,uint16_t count,ksn_template *out){
    if(!runtime||!v||v->host!=&runtime->host||!out)return KSN_INVALID;
    if(runtime->host.presenting||runtime->host.builder.value||ksn_core_has_submission(&runtime->core))return KSN_BUSY;
    if(runtime->cache)return ksn_view_cache_create(v,draws,count,out);
    ksn_cache *cache=calloc(1,sizeof(*cache));if(!cache)return KSN_OOM;
    ksn_cache_command_block *commands=calloc(1,sizeof(*commands));
    if(!commands){free(cache);return KSN_OOM;}
    ksn_cache_text_block *text=calloc(1,sizeof(*text));
    if(!text){free(commands);free(cache);return KSN_OOM;}
    ksn_cache_bind(cache,commands,text);
    ksn_template candidate={0};
    ksn_result r=ksn_cache_create(cache,v->layer,draws,count,&candidate);
    if(r==KSN_OK)r=ksn_view_host_attach_cache(&runtime->host,cache);
    if(r!=KSN_OK){free_cache(cache);return r;}
    runtime->cache=cache;*out=candidate;return KSN_OK;
}
ksn_result ksn_runtime_animate(ksn_view *v,ksn_tx tx,const ksn_motion *m,ksn_animation *out){
    if(!runtime||!v||v->host!=&runtime->host||!out)return KSN_INVALID;
    if(runtime->host.presenting||!tx.value||runtime->host.builder.value!=tx.value||runtime->host.building_layer!=v->layer)return KSN_STALE;
    if(!runtime->animations){
        ksn_core_animation_block *blocks=calloc(2,sizeof(*blocks));
        if(!blocks){ksn_view_cancel(v,tx);return KSN_OOM;}
        ksn_result r=ksn_core_enable_animation(&runtime->core,&blocks[0],&blocks[1]);
        if(r!=KSN_OK){free(blocks);ksn_view_cancel(v,tx);return r;}runtime->animations=blocks;
    }
    return ksn_view_animate(v,tx,m,out);
}
ksn_result ksn_runtime_advance_animations(uint64_t now){
    return runtime&&!runtime->hidden?ksn_view_host_advance_animations(&runtime->host,now,runtime->reduce_motion):KSN_OK;
}
void ksn_runtime_animations_presented(uint64_t now){if(runtime)ksn_core_start_animations(&runtime->core,now);}
void ksn_runtime_set_animation_time(uint64_t now){if(runtime&&!runtime->hidden)ksn_core_set_animation_time(&runtime->core,now);}
uint64_t ksn_runtime_animation_deadline(void){
    uint64_t next=runtime&&!runtime->hidden?ksn_core_animation_deadline(&runtime->core):UINT64_MAX;
    return next!=UINT64_MAX&&runtime->reduce_motion?0:next;
}
void ksn_runtime_set_hidden(bool hidden){if(runtime)runtime->hidden=hidden;}
void ksn_runtime_set_reduce_motion(bool enabled){if(runtime)runtime->reduce_motion=enabled;}
bool ksn_runtime_animation_pending(void){return runtime&&runtime->host.animation_submission.value;}
uint32_t ksn_runtime_cache_bytes(void){return runtime&&runtime->cache?KSN_CACHE_RESERVED_BYTES:0;}
ksn_view_stats ksn_runtime_stats(ksn_layer layer){
    return runtime?ksn_view_get_stats(ksn_view_host_endpoint(&runtime->host,layer)):(ksn_view_stats){0};
}
uint32_t ksn_runtime_reserved_bytes(void){return runtime?BASE_BYTES+ksn_runtime_cache_bytes()+ksn_core_animation_bytes(&runtime->core):0;}
bool ksn_runtime_has_submission(void){return runtime&&ksn_core_has_submission(&runtime->core);}
bool ksn_runtime_needs_present(void){
    return runtime&&(runtime->system_owned||runtime->app_active)&&ksn_view_host_needs_present(&runtime->host);
}
void ksn_runtime_invalidate(void){if(runtime)ksn_view_host_invalidate(&runtime->host);}
void ksn_runtime_invalidate_bands(uint32_t bands){
    if(runtime)ksn_view_host_invalidate_bands(&runtime->host,bands);
}
ksn_result ksn_runtime_present(const ksn_display_port *port,ksn_render_stats *stats){
    if(!stats)return KSN_INVALID;
    *stats=(ksn_render_stats){0};
    return ksn_runtime_needs_present()?ksn_view_host_present(&runtime->host,port,stats):KSN_OK;
}
ksn_result ksn_runtime_present_backdrop(const ksn_display_port *port,ksn_backdrop_loader load,
                                        ksn_render_stats *stats){
    if(!stats)return KSN_INVALID;
    *stats=(ksn_render_stats){0};
    return ksn_runtime_needs_present()?
        ksn_view_host_present_backdrop(&runtime->host,port,load,stats):KSN_OK;
}
ksn_input_scope ksn_runtime_input_scope(bool priority){
    if(!runtime)return priority?KSN_INPUT_HOST:KSN_INPUT_APP;
    if(!runtime->app.value)return KSN_INPUT_HOST;
    return ksn_view_host_route(&runtime->host,priority);
}
