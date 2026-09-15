#include "ksn_runtime.h"
#include <stdlib.h>

typedef struct {
    ksn_core core;
    ksn_view_host host;
    ksn_cache *cache;
    ksn_app_lease app;
    bool system_owned,app_active;
} runtime_storage;
static runtime_storage *runtime;
/* Process-lifetime identities prevent leases reviving after heap address reuse. */
static uint32_t last_lease;
_Static_assert(sizeof(runtime_storage)<=3072,"runtime control allocation budget");
#define BASE_BYTES (sizeof(runtime_storage)+2*sizeof(ksn_core_command_block)+2*sizeof(ksn_core_text_block))

static void free_cache(ksn_cache *cache){
    if(cache){free(cache->state.commands);free(cache->state.text);free(cache);}
}
static void destroy(void){
    free_cache(runtime->cache);
    for(unsigned i=0;i<2;i++){
        free(runtime->core.state.banks[i].commands);
        free(runtime->core.state.banks[i].text);
    }
    free(runtime);runtime=NULL;
}
static ksn_result ensure(void){
    if(runtime)return KSN_OK;
    runtime_storage *r=calloc(1,sizeof(*r));
    ksn_core_command_block *commands[2]={NULL,NULL};
    ksn_core_text_block *text[2]={NULL,NULL};
    if(!r)goto fail;
    for(unsigned i=0;i<2;i++){
        commands[i]=calloc(1,sizeof(*commands[i]));if(!commands[i])goto fail;
        text[i]=calloc(1,sizeof(*text[i]));if(!text[i])goto fail;
    }
    ksn_core_bind(&r->core,commands[0],commands[1],text[0],text[1]);
    ksn_view_host_init(&r->host,&r->core,NULL,0);
    runtime=r;return KSN_OK;
fail:
    for(unsigned i=0;i<2;i++){free(commands[i]);free(text[i]);}
    free(r);return KSN_OOM;
}
ksn_view *ksn_runtime_app_view(ksn_app_lease lease){
    return runtime&&lease.value&&lease.value==runtime->app.value?
           ksn_view_host_endpoint(&runtime->host,KSN_APP):NULL;
}
ksn_result ksn_runtime_app_attach(ksn_app_lease *out){
    if(!out)return KSN_INVALID;
    if(runtime&&(runtime->app.value||runtime->host.presenting))return KSN_BUSY;
    if(last_lease==UINT32_MAX)return KSN_LIMIT;
    ksn_result r=ensure();if(r!=KSN_OK)return r;
    runtime->app=(ksn_app_lease){++last_lease};*out=runtime->app;return KSN_OK;
}
ksn_result ksn_runtime_app_detach(ksn_app_lease lease){
    if(!ksn_runtime_app_view(lease))return KSN_STALE;
    ksn_result r=ksn_view_host_reset_app(&runtime->host);if(r!=KSN_OK)return r;
    runtime->app=(ksn_app_lease){0};runtime->app_active=false;
    if(!runtime->system_owned)destroy();
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
    ksn_result r=ensure();if(r!=KSN_OK)return r;
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
uint32_t ksn_runtime_cache_bytes(void){return runtime&&runtime->cache?KSN_CACHE_RESERVED_BYTES:0;}
ksn_view_stats ksn_runtime_stats(ksn_layer layer){
    return runtime?ksn_view_get_stats(ksn_view_host_endpoint(&runtime->host,layer)):(ksn_view_stats){0};
}
uint32_t ksn_runtime_reserved_bytes(void){return runtime?BASE_BYTES+ksn_runtime_cache_bytes():0;}
bool ksn_runtime_has_submission(void){return runtime&&ksn_core_has_submission(&runtime->core);}
bool ksn_runtime_needs_present(void){
    return runtime&&(runtime->system_owned||runtime->app_active)&&ksn_view_host_needs_present(&runtime->host);
}
void ksn_runtime_invalidate(void){if(runtime)ksn_view_host_invalidate(&runtime->host);}
ksn_result ksn_runtime_present(const ksn_display_port *port,ksn_render_stats *stats){
    if(!stats)return KSN_INVALID;
    *stats=(ksn_render_stats){0};
    return ksn_runtime_needs_present()?ksn_view_host_present(&runtime->host,port,stats):KSN_OK;
}
ksn_input_scope ksn_runtime_input_scope(bool priority){
    if(!runtime)return priority?KSN_INPUT_HOST:KSN_INPUT_APP;
    if(!runtime->app.value)return KSN_INPUT_HOST;
    return ksn_view_host_route(&runtime->host,priority);
}
