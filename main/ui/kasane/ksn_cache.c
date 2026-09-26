#include "ksn_cache.h"
#include <string.h>

#define INSTANCE_PENDING_NEW 1u
static uint32_t last_template,last_instance;
typedef struct { ksn_rgba color; uint8_t radius,width,pad[2]; } shape_payload;

_Static_assert(KSN_CACHE_RESERVED_BYTES<=KSN_CACHE_STORAGE_BYTES,"cache storage budget");
_Static_assert(sizeof(ksn_cache)<=3072,"cache metadata allocation budget");
_Static_assert(sizeof(ksn_cache_command_block)<=3072,"cache command allocation budget");
_Static_assert(sizeof(ksn_cache_text_block)<=3072,"cache text allocation budget");
_Static_assert(sizeof(shape_payload)<=sizeof(((ksn_command_storage *)0)->payload),"shape payload");

static bool valid_layer(ksn_layer layer){return layer==KSN_APP||layer==KSN_SYSTEM;}
static bool bound(const ksn_cache *cache){return cache&&cache->state.commands&&cache->state.text;}
static bool valid_rect(ksn_rect r){return r.x0<=r.x1&&r.y0<=r.y1;}
static unsigned width(ksn_rect r){return (unsigned)((int32_t)r.x1-r.x0);}
static unsigned height(ksn_rect r){return (unsigned)((int32_t)r.y1-r.y0);}
static bool valid_radius(ksn_rect r,uint8_t radius){
    return radius<=8u&&radius<=width(r)/2u&&radius<=height(r)/2u;
}
static ksn_cache_template_entry *find_template(ksn_cache_impl *cache,ksn_template handle){
    for(unsigned i=0;i<cache->template_count;i++)if(cache->templates[i].id==handle.value)return &cache->templates[i];
    return NULL;
}
static const ksn_cache_template_entry *find_template_const(const ksn_cache_impl *cache,uint32_t id){
    for(unsigned i=0;i<cache->template_count;i++)if(cache->templates[i].id==id)return &cache->templates[i];
    return NULL;
}
static ksn_cache_instance_entry *find_instance(ksn_cache_impl *cache,ksn_instance handle){
    for(unsigned i=0;i<cache->instance_count;i++)if(cache->instances[i].id==handle.value)return &cache->instances[i];
    return NULL;
}
static ksn_result validate_placement(const ksn_placement *p){
    if(!p||!valid_rect(p->clip))return KSN_INVALID;
    return KSN_OK;
}
static bool translate_rect(ksn_rect in,int16_t x,int16_t y,ksn_rect *out){
    int32_t x0=(int32_t)in.x0+x,y0=(int32_t)in.y0+y,x1=(int32_t)in.x1+x,y1=(int32_t)in.y1+y;
    if(x0<INT16_MIN||y0<INT16_MIN||x1>INT16_MAX||y1>INT16_MAX)return false;
    *out=(ksn_rect){(int16_t)x0,(int16_t)y0,(int16_t)x1,(int16_t)y1};return true;
}
/* The template extent has already proved that every translated endpoint fits. */
static ksn_rect translate_checked_extent(ksn_rect in,int16_t x,int16_t y){
    return (ksn_rect){(int16_t)((int32_t)in.x0+x),(int16_t)((int32_t)in.y0+y),
                      (int16_t)((int32_t)in.x1+x),(int16_t)((int32_t)in.y1+y)};
}
static bool same_rect(ksn_rect a,ksn_rect b){
    return a.x0==b.x0&&a.y0==b.y0&&a.x1==b.x1&&a.y1==b.y1;
}
static ksn_rect intersect(ksn_rect a,ksn_rect b){
    if(a.x0<b.x0)a.x0=b.x0;
    if(a.y0<b.y0)a.y0=b.y0;
    if(a.x1>b.x1)a.x1=b.x1;
    if(a.y1>b.y1)a.y1=b.y1;
    if(a.x1<a.x0)a.x1=a.x0;
    if(a.y1<a.y0)a.y1=a.y0;
    return a;
}
static ksn_result decode(const ksn_command_storage *stored,ksn_draw *draw){
    memset(draw,0,sizeof(*draw));draw->kind=(ksn_kind)stored->kind;draw->bounds=stored->bounds;
    draw->clip=stored->clip;draw->opacity=stored->opacity;
    if(draw->kind!=KSN_RECT&&draw->kind!=KSN_ROUND_RECT&&draw->kind!=KSN_STROKE)return KSN_UNSUPPORTED;
    shape_payload payload;memcpy(&payload,stored->payload,sizeof(payload));
    draw->data.shape.color=payload.color;draw->data.shape.radius=payload.radius;
    draw->data.shape.width=payload.width;return KSN_OK;
}
static ksn_result apply_placement(ksn_cache_impl *cache,ksn_core *core,ksn_tx tx,
                                 ksn_cache_instance_entry *instance,const ksn_placement *placement){
    ksn_result result=validate_placement(placement);if(result!=KSN_OK)return result;
    const ksn_cache_template_entry *entry=find_template_const(cache,instance->template_id);
    if(!entry)return KSN_STALE;
    ksn_rect translated;
    if(!translate_rect(entry->extent,placement->x,placement->y,&translated))return KSN_INVALID;
    const ksn_placement *previous=instance->pending_tx==tx.value?&instance->pending:&instance->current;
    bool moved=previous->x!=placement->x||previous->y!=placement->y;
    bool clip_changed=moved||!same_rect(previous->clip,placement->clip);
    bool visibility_changed=previous->visible!=placement->visible;
    bool opacity_changed=previous->opacity!=placement->opacity;
    if(!moved&&!clip_changed&&!visibility_changed&&!opacity_changed){
        /* A no-op must still reject a stale, wrong-layer or poisoned ticket. */
        result=ksn_core_check_transaction(core,tx,(ksn_layer)entry->layer);
        if(result!=KSN_OK)return result;
        /* REPLACE may have invalidated an old instance's refs. PATCH keeps
         * them, but REPLACE must validate the group before accepting no-op. */
        if(ksn_core_check_builder(core,tx,(ksn_layer)entry->layer,KSN_REPLACE)==KSN_OK){
            result=ksn_core_group(core,(ksn_layer)entry->layer,tx,instance->first,
                                  entry->command_count,placement->opacity);
            if(result!=KSN_OK)return result;
        }
        instance->pending=*placement;instance->pending_tx=tx.value;return KSN_OK;
    }
    ksn_client client=ksn_core_client(core,(ksn_layer)entry->layer);
    if(moved||clip_changed||visibility_changed)for(unsigned i=0;i<entry->command_count;i++){
        const ksn_command_storage *stored=&cache->commands[entry->first_command+i];
        ksn_ref ref={instance->first.value+i};ksn_change change;
        if(moved){
            change=(ksn_change){.property=KSN_SET_RECT,
                .value.rect=translate_checked_extent(stored->bounds,placement->x,placement->y)};
            result=client.ops->change(client.ctx,tx,ref,&change);if(result!=KSN_OK)return result;
        }
        if(clip_changed){
            ksn_rect clip=translate_checked_extent(stored->clip,placement->x,placement->y);
            change=(ksn_change){.property=KSN_SET_CLIP,.value.rect=intersect(clip,placement->clip)};
            result=client.ops->change(client.ctx,tx,ref,&change);if(result!=KSN_OK)return result;
        }
        if(visibility_changed){
            change=(ksn_change){.property=KSN_SET_VISIBLE,.value.visible=placement->visible};
            result=client.ops->change(client.ctx,tx,ref,&change);if(result!=KSN_OK)return result;
        }
    }
    if(opacity_changed){
        result=ksn_core_group(core,(ksn_layer)entry->layer,tx,instance->first,entry->command_count,placement->opacity);
        if(result!=KSN_OK)return result;
    }
    instance->pending=*placement;instance->pending_tx=tx.value;return KSN_OK;
}

void ksn_cache_init(ksn_cache *storage){
    if(!bound(storage))return;
    ksn_command_storage *commands=storage->state.commands;uint8_t *text=storage->state.text;
    *storage=(ksn_cache){.state={.commands=commands,.text=text}};
    memset(commands,0,sizeof(ksn_cache_command_block));memset(text,0,sizeof(ksn_cache_text_block));
}
ksn_result ksn_cache_bind(ksn_cache *storage,ksn_cache_command_block *commands,ksn_cache_text_block *text){
    if(!storage||!commands||!text)return KSN_INVALID;
    *storage=(ksn_cache){.state={.commands=commands->commands,.text=text->bytes}};
    ksn_cache_init(storage);return KSN_OK;
}
ksn_result ksn_cache_create(ksn_cache *storage,ksn_layer layer,const ksn_draw *draws,
                          uint16_t count,ksn_template *out){
    if(!bound(storage)||!valid_layer(layer)||!draws||!count||!out)return KSN_INVALID;
    ksn_cache_impl *cache=&storage->state;
    if(cache->template_count==KSN_CACHE_TEMPLATES||count>KSN_CACHE_COMMANDS-cache->command_used||
       last_template==UINT32_MAX)return KSN_LIMIT;
    for(unsigned i=0;i<count;i++){
        const ksn_draw *draw=&draws[i];
        if(!valid_rect(draw->bounds)||!valid_rect(draw->clip))return KSN_INVALID;
        if(draw->kind==KSN_RECT){if(draw->data.shape.radius||draw->data.shape.width)return KSN_INVALID;}
        else if(draw->kind==KSN_ROUND_RECT){if(draw->data.shape.width||!valid_radius(draw->bounds,draw->data.shape.radius))return KSN_INVALID;}
        else if(draw->kind==KSN_STROKE){if((draw->data.shape.width!=1&&draw->data.shape.width!=2)||draw->data.shape.radius)return KSN_INVALID;}
        else return KSN_UNSUPPORTED;
    }
    unsigned first=cache->command_used;
    for(unsigned i=0;i<count;i++){
        const ksn_draw *draw=&draws[i];ksn_command_storage *stored=&cache->commands[cache->command_used++];
        *stored=(ksn_command_storage){.kind=(uint8_t)draw->kind,.flags=draw->opacity?1:0,
                                     .opacity=draw->opacity,.bounds=draw->bounds,.clip=draw->clip};
        shape_payload payload={draw->data.shape.color,draw->data.shape.radius,draw->data.shape.width,{0,0}};
        memcpy(stored->payload,&payload,sizeof(payload));
    }
    ksn_rect extent={INT16_MAX,INT16_MAX,INT16_MIN,INT16_MIN};
    for(unsigned i=0;i<count;i++){
        const ksn_rect rects[2]={draws[i].bounds,draws[i].clip};
        for(unsigned j=0;j<2;j++){
            ksn_rect r=rects[j];
            if(r.x0<extent.x0)extent.x0=r.x0;
            if(r.y0<extent.y0)extent.y0=r.y0;
            if(r.x1>extent.x1)extent.x1=r.x1;
            if(r.y1>extent.y1)extent.y1=r.y1;
        }
    }
    ksn_cache_template_entry *entry=&cache->templates[cache->template_count++];
    *entry=(ksn_cache_template_entry){++last_template,(uint16_t)first,0,0,(uint8_t)count,(uint8_t)layer,extent};
    *out=(ksn_template){entry->id};return KSN_OK;
}
ksn_result ksn_cache_release(ksn_cache *storage,ksn_template handle){
    if(!bound(storage)||!handle.value)return KSN_INVALID;
    ksn_cache_impl *cache=&storage->state;
    ksn_cache_template_entry *entry=find_template(cache,handle);
    if(!entry)return KSN_STALE;
    for(unsigned i=0;i<cache->instance_count;i++)if(cache->instances[i].template_id==handle.value)return KSN_BUSY;
    unsigned template_index=(unsigned)(entry-cache->templates),first=entry->first_command,count=entry->command_count;
    memmove(cache->commands+first,cache->commands+first+count,
            (cache->command_used-first-count)*sizeof(ksn_command_storage));cache->command_used-=count;
    for(unsigned i=0;i<cache->template_count;i++)if(cache->templates[i].first_command>first)cache->templates[i].first_command-=count;
    memmove(entry,entry+1,(cache->template_count-template_index-1)*sizeof(*entry));cache->template_count--;
    return KSN_OK;
}
void ksn_cache_reset_layer(ksn_cache *storage,ksn_layer layer){
    if(!bound(storage)||!valid_layer(layer))return;
    ksn_cache_impl *cache=&storage->state;
    unsigned kept=0;
    for(unsigned i=0;i<cache->instance_count;i++)
        if(cache->instances[i].layer!=layer)cache->instances[kept++]=cache->instances[i];
    memset(cache->instances+kept,0,(cache->instance_count-kept)*sizeof(*cache->instances));
    cache->instance_count=(uint8_t)kept;
    for(unsigned i=0;i<cache->template_count;){
        if(cache->templates[i].layer!=layer){i++;continue;}
        /* All instances of these templates were removed above. */
        ksn_cache_release(storage,(ksn_template){cache->templates[i].id});
    }
}
ksn_result ksn_cache_instantiate(ksn_cache *storage,ksn_core *core,ksn_tx tx,
                               ksn_template handle,const ksn_placement *placement,ksn_instance *out){
    if(!bound(storage)||!core||!out)return KSN_INVALID;
    ksn_result result=validate_placement(placement);
    if(result!=KSN_OK)return result;
    ksn_cache_impl *cache=&storage->state;
    ksn_cache_template_entry *entry=find_template(cache,handle);
    if(!entry)return KSN_STALE;
    if(cache->instance_count==KSN_CACHE_INSTANCES||last_instance==UINT32_MAX)return KSN_LIMIT;
    ksn_rect translated;
    if(!translate_rect(entry->extent,placement->x,placement->y,&translated))return KSN_INVALID;
    ksn_client client=ksn_core_client(core,(ksn_layer)entry->layer);ksn_cache_instance_entry pending={0};
    pending.id=++last_instance;pending.template_id=entry->id;pending.layer=entry->layer;
    pending.command_count=entry->command_count;pending.pending=*placement;pending.pending_tx=tx.value;
    pending.flags=INSTANCE_PENDING_NEW;
    for(unsigned i=0;i<entry->command_count;i++){
        ksn_draw draw;result=decode(&cache->commands[entry->first_command+i],&draw);if(result!=KSN_OK)return result;
        draw.bounds=translate_checked_extent(draw.bounds,placement->x,placement->y);
        draw.clip=translate_checked_extent(draw.clip,placement->x,placement->y);
        draw.clip=intersect(draw.clip,placement->clip);
        ksn_ref ref;result=client.ops->add(client.ctx,tx,&draw,&ref);if(result!=KSN_OK)return result;
        if(!placement->visible){
            ksn_change change={.property=KSN_SET_VISIBLE,.value.visible=false};
            result=client.ops->change(client.ctx,tx,ref,&change);if(result!=KSN_OK)return result;
        }
        if(!i)pending.first=ref;
    }
    result=ksn_core_group(core,(ksn_layer)entry->layer,tx,pending.first,entry->command_count,placement->opacity);
    if(result!=KSN_OK)return result;
    cache->instances[cache->instance_count++]=pending;*out=(ksn_instance){pending.id};return KSN_OK;
}
ksn_result ksn_cache_place(ksn_cache *storage,ksn_core *core,ksn_tx tx,
                         ksn_instance handle,const ksn_placement *placement){
    if(!bound(storage)||!core)return KSN_INVALID;
    ksn_cache_impl *cache=&storage->state;
    ksn_cache_instance_entry *instance=find_instance(cache,handle);
    if(!instance)return KSN_STALE;
    return apply_placement(cache,core,tx,instance,placement);
}
ksn_result ksn_cache_set_visible(ksn_cache *storage,ksn_core *core,ksn_tx tx,ksn_instance handle,bool visible){
    if(!bound(storage)||!core)return KSN_INVALID;
    ksn_cache_impl *cache=&storage->state;
    ksn_cache_instance_entry *instance=find_instance(cache,handle);
    if(!instance)return KSN_STALE;
    ksn_placement placement=instance->pending_tx==tx.value?instance->pending:instance->current;
    placement.visible=visible;return apply_placement(cache,core,tx,instance,&placement);
}
ksn_result ksn_cache_abort(ksn_cache *storage,ksn_tx ticket){
    if(!bound(storage)||!ticket.value)return KSN_INVALID;
    ksn_cache_impl *cache=&storage->state;bool found=false;
    for(unsigned i=0;i<cache->instance_count;){
        ksn_cache_instance_entry *instance=&cache->instances[i];
        if(instance->pending_tx!=ticket.value){i++;continue;}
        found=true;
        if(instance->flags&INSTANCE_PENDING_NEW){
            memmove(instance,instance+1,(cache->instance_count-i-1)*sizeof(*instance));
            cache->instance_count--;continue;
        }
        instance->pending_tx=0;i++;
    }
    return found?KSN_OK:KSN_STALE;
}
ksn_result ksn_cache_resolve(ksn_cache *storage,const ksn_core *core,ksn_tx ticket,bool presented){
    if(!bound(storage)||!core||!ticket.value)return KSN_INVALID;
    if(ksn_core_has_submission(core))return KSN_BUSY;
    ksn_submission outcome=ksn_core_poll(core);
    if(outcome.ticket.value!=ticket.value||
       outcome.status!=(presented?KSN_PRESENTED:KSN_DISCARDED))return KSN_STALE;
    ksn_cache_impl *cache=&storage->state;bool found=false,pruned=false;
    for(unsigned i=0;i<cache->instance_count;){
        ksn_cache_instance_entry *instance=&cache->instances[i];
        if(instance->pending_tx==ticket.value){
            found=true;
            if(!presented&&(instance->flags&INSTANCE_PENDING_NEW)){
                memmove(instance,instance+1,(cache->instance_count-i-1)*sizeof(*instance));cache->instance_count--;continue;
            }
            if(presented)instance->current=instance->pending;
            instance->pending_tx=0;instance->flags&=(uint8_t)~INSTANCE_PENDING_NEW;
        }
        if(presented&&!ksn_core_refs_active(core,(ksn_layer)instance->layer,instance->first,instance->command_count)){
            memmove(instance,instance+1,(cache->instance_count-i-1)*sizeof(*instance));
            cache->instance_count--;pruned=true;continue;
        }
        i++;
    }
    return found||pruned?KSN_OK:KSN_STALE;
}
ksn_cache_stats ksn_cache_get_stats(const ksn_cache *storage){
    if(!bound(storage))return (ksn_cache_stats){0};
    const ksn_cache_impl *cache=&storage->state;
    return (ksn_cache_stats){cache->command_used,cache->text_used,cache->template_count,cache->instance_count,
                            KSN_CACHE_RESERVED_BYTES+sizeof(last_template)+sizeof(last_instance)};
}
