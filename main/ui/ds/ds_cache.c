#include "ds_cache.h"
#include <string.h>

#define INSTANCE_PENDING_NEW 1u
static uint32_t last_template,last_instance;
typedef struct { ds_rgba color; uint8_t radius,width,pad[2]; } shape_payload;

_Static_assert(sizeof(ds_cache_impl)<=DS_CACHE_STORAGE_BYTES,"cache storage budget");
_Static_assert(sizeof(shape_payload)<=sizeof(((ds_command_storage *)0)->payload),"shape payload");

static bool valid_layer(ds_layer layer){return layer==DS_APP||layer==DS_SYSTEM;}
static bool valid_rect(ds_rect r){return r.x0<=r.x1&&r.y0<=r.y1;}
static unsigned width(ds_rect r){return (unsigned)((int32_t)r.x1-r.x0);}
static unsigned height(ds_rect r){return (unsigned)((int32_t)r.y1-r.y0);}
static bool valid_radius(ds_rect r,uint8_t radius){
    return radius<=8u&&radius<=width(r)/2u&&radius<=height(r)/2u;
}
static ds_cache_template_entry *find_template(ds_cache_impl *cache,ds_template handle){
    for(unsigned i=0;i<cache->template_count;i++)if(cache->templates[i].id==handle.value)return &cache->templates[i];
    return NULL;
}
static const ds_cache_template_entry *find_template_const(const ds_cache_impl *cache,uint32_t id){
    for(unsigned i=0;i<cache->template_count;i++)if(cache->templates[i].id==id)return &cache->templates[i];
    return NULL;
}
static ds_cache_instance_entry *find_instance(ds_cache_impl *cache,ds_instance handle){
    for(unsigned i=0;i<cache->instance_count;i++)if(cache->instances[i].id==handle.value)return &cache->instances[i];
    return NULL;
}
static ds_result validate_placement(const ds_placement *p){
    if(!p||!valid_rect(p->clip))return DS_INVALID;
    return p->opacity==255?DS_OK:DS_UNSUPPORTED;
}
static bool translate_rect(ds_rect in,int16_t x,int16_t y,ds_rect *out){
    int32_t x0=(int32_t)in.x0+x,y0=(int32_t)in.y0+y,x1=(int32_t)in.x1+x,y1=(int32_t)in.y1+y;
    if(x0<INT16_MIN||y0<INT16_MIN||x1>INT16_MAX||y1>INT16_MAX)return false;
    *out=(ds_rect){(int16_t)x0,(int16_t)y0,(int16_t)x1,(int16_t)y1};return true;
}
static ds_rect intersect(ds_rect a,ds_rect b){
    if(a.x0<b.x0)a.x0=b.x0;
    if(a.y0<b.y0)a.y0=b.y0;
    if(a.x1>b.x1)a.x1=b.x1;
    if(a.y1>b.y1)a.y1=b.y1;
    if(a.x1<a.x0)a.x1=a.x0;
    if(a.y1<a.y0)a.y1=a.y0;
    return a;
}
static ds_result decode(const ds_command_storage *stored,ds_draw *draw){
    memset(draw,0,sizeof(*draw));draw->kind=(ds_kind)stored->kind;draw->bounds=stored->bounds;
    draw->clip=stored->clip;draw->opacity=stored->opacity;
    if(draw->kind!=DS_RECT&&draw->kind!=DS_ROUND_RECT&&draw->kind!=DS_STROKE)return DS_UNSUPPORTED;
    shape_payload payload;memcpy(&payload,stored->payload,sizeof(payload));
    draw->data.shape.color=payload.color;draw->data.shape.radius=payload.radius;
    draw->data.shape.width=payload.width;return DS_OK;
}
static ds_result apply_placement(ds_cache_impl *cache,ds_core *core,ds_tx tx,
                                 ds_cache_instance_entry *instance,const ds_placement *placement){
    ds_result result=validate_placement(placement);if(result!=DS_OK)return result;
    const ds_cache_template_entry *entry=find_template_const(cache,instance->template_id);
    if(!entry)return DS_STALE;
    for(unsigned i=0;i<entry->command_count;i++){
        ds_draw draw;ds_rect translated;
        result=decode(&cache->commands[entry->first_command+i],&draw);
        if(result!=DS_OK)return result;
        if(!translate_rect(draw.bounds,placement->x,placement->y,&translated)||
           !translate_rect(draw.clip,placement->x,placement->y,&translated))return DS_INVALID;
    }
    ds_client client=ds_core_client(core,(ds_layer)entry->layer);
    for(unsigned i=0;i<entry->command_count;i++){
        ds_draw draw;result=decode(&cache->commands[entry->first_command+i],&draw);
        if(result!=DS_OK)return result;
        ds_rect bounds,clip;
        if(!translate_rect(draw.bounds,placement->x,placement->y,&bounds)||
           !translate_rect(draw.clip,placement->x,placement->y,&clip))return DS_INVALID;
        clip=intersect(clip,placement->clip);
        ds_ref ref={instance->first.value+i};
        ds_change change={.property=DS_SET_RECT,.value.rect=bounds};
        result=client.ops->change(client.ctx,tx,ref,&change);if(result!=DS_OK)return result;
        change=(ds_change){.property=DS_SET_CLIP,.value.rect=clip};
        result=client.ops->change(client.ctx,tx,ref,&change);if(result!=DS_OK)return result;
        change=(ds_change){.property=DS_SET_VISIBLE,.value.visible=placement->visible};
        result=client.ops->change(client.ctx,tx,ref,&change);if(result!=DS_OK)return result;
    }
    instance->pending=*placement;instance->pending_tx=tx.value;return DS_OK;
}

void ds_cache_init(ds_cache *storage){if(storage)memset(storage,0,sizeof(*storage));}
ds_result ds_cache_create(ds_cache *storage,ds_layer layer,const ds_draw *draws,
                          uint16_t count,ds_template *out){
    if(!storage||!valid_layer(layer)||!draws||!count||!out)return DS_INVALID;
    ds_cache_impl *cache=&storage->state;
    if(cache->template_count==DS_CACHE_TEMPLATES||count>DS_CACHE_COMMANDS-cache->command_used||
       last_template==UINT32_MAX)return DS_LIMIT;
    for(unsigned i=0;i<count;i++){
        const ds_draw *draw=&draws[i];
        if(!valid_rect(draw->bounds)||!valid_rect(draw->clip))return DS_INVALID;
        if(draw->kind==DS_RECT){if(draw->data.shape.radius||draw->data.shape.width)return DS_INVALID;}
        else if(draw->kind==DS_ROUND_RECT){if(draw->data.shape.width||!valid_radius(draw->bounds,draw->data.shape.radius))return DS_INVALID;}
        else if(draw->kind==DS_STROKE){if((draw->data.shape.width!=1&&draw->data.shape.width!=2)||draw->data.shape.radius)return DS_INVALID;}
        else return DS_UNSUPPORTED;
    }
    unsigned first=cache->command_used;
    for(unsigned i=0;i<count;i++){
        const ds_draw *draw=&draws[i];ds_command_storage *stored=&cache->commands[cache->command_used++];
        *stored=(ds_command_storage){.kind=(uint8_t)draw->kind,.flags=draw->opacity?1:0,
                                     .opacity=draw->opacity,.bounds=draw->bounds,.clip=draw->clip};
        shape_payload payload={draw->data.shape.color,draw->data.shape.radius,draw->data.shape.width,{0,0}};
        memcpy(stored->payload,&payload,sizeof(payload));
    }
    ds_cache_template_entry *entry=&cache->templates[cache->template_count++];
    *entry=(ds_cache_template_entry){++last_template,(uint16_t)first,0,0,(uint8_t)count,(uint8_t)layer};
    *out=(ds_template){entry->id};return DS_OK;
}
ds_result ds_cache_release(ds_cache *storage,ds_template handle){
    if(!storage||!handle.value)return DS_INVALID;
    ds_cache_impl *cache=&storage->state;
    ds_cache_template_entry *entry=find_template(cache,handle);
    if(!entry)return DS_STALE;
    for(unsigned i=0;i<cache->instance_count;i++)if(cache->instances[i].template_id==handle.value)return DS_BUSY;
    unsigned template_index=(unsigned)(entry-cache->templates),first=entry->first_command,count=entry->command_count;
    memmove(cache->commands+first,cache->commands+first+count,
            (cache->command_used-first-count)*sizeof(ds_command_storage));cache->command_used-=count;
    for(unsigned i=0;i<cache->template_count;i++)if(cache->templates[i].first_command>first)cache->templates[i].first_command-=count;
    memmove(entry,entry+1,(cache->template_count-template_index-1)*sizeof(*entry));cache->template_count--;
    return DS_OK;
}
ds_result ds_cache_instantiate(ds_cache *storage,ds_core *core,ds_tx tx,
                               ds_template handle,const ds_placement *placement,ds_instance *out){
    if(!storage||!core||!out)return DS_INVALID;
    ds_result result=validate_placement(placement);
    if(result!=DS_OK)return result;
    ds_cache_impl *cache=&storage->state;
    ds_cache_template_entry *entry=find_template(cache,handle);
    if(!entry)return DS_STALE;
    if(cache->instance_count==DS_CACHE_INSTANCES||last_instance==UINT32_MAX)return DS_LIMIT;
    for(unsigned i=0;i<entry->command_count;i++){
        ds_draw draw;ds_rect translated;
        result=decode(&cache->commands[entry->first_command+i],&draw);
        if(result!=DS_OK)return result;
        if(!translate_rect(draw.bounds,placement->x,placement->y,&translated)||
           !translate_rect(draw.clip,placement->x,placement->y,&translated))return DS_INVALID;
    }
    ds_client client=ds_core_client(core,(ds_layer)entry->layer);ds_cache_instance_entry pending={0};
    pending.id=++last_instance;pending.template_id=entry->id;pending.layer=entry->layer;
    pending.command_count=entry->command_count;pending.pending=*placement;pending.pending_tx=tx.value;
    pending.flags=INSTANCE_PENDING_NEW;
    for(unsigned i=0;i<entry->command_count;i++){
        ds_draw draw;result=decode(&cache->commands[entry->first_command+i],&draw);if(result!=DS_OK)return result;
        if(!translate_rect(draw.bounds,placement->x,placement->y,&draw.bounds)||
           !translate_rect(draw.clip,placement->x,placement->y,&draw.clip))return DS_INVALID;
        draw.clip=intersect(draw.clip,placement->clip);
        ds_ref ref;result=client.ops->add(client.ctx,tx,&draw,&ref);if(result!=DS_OK)return result;
        if(!placement->visible){
            ds_change change={.property=DS_SET_VISIBLE,.value.visible=false};
            result=client.ops->change(client.ctx,tx,ref,&change);if(result!=DS_OK)return result;
        }
        if(!i)pending.first=ref;
    }
    cache->instances[cache->instance_count++]=pending;*out=(ds_instance){pending.id};return DS_OK;
}
ds_result ds_cache_place(ds_cache *storage,ds_core *core,ds_tx tx,
                         ds_instance handle,const ds_placement *placement){
    if(!storage||!core)return DS_INVALID;
    ds_cache_impl *cache=&storage->state;
    ds_cache_instance_entry *instance=find_instance(cache,handle);
    if(!instance)return DS_STALE;
    return apply_placement(cache,core,tx,instance,placement);
}
ds_result ds_cache_set_visible(ds_cache *storage,ds_core *core,ds_tx tx,ds_instance handle,bool visible){
    if(!storage||!core)return DS_INVALID;
    ds_cache_impl *cache=&storage->state;
    ds_cache_instance_entry *instance=find_instance(cache,handle);
    if(!instance)return DS_STALE;
    ds_placement placement=instance->pending_tx==tx.value?instance->pending:instance->current;
    placement.visible=visible;return apply_placement(cache,core,tx,instance,&placement);
}
ds_result ds_cache_abort(ds_cache *storage,ds_tx ticket){
    if(!storage||!ticket.value)return DS_INVALID;
    ds_cache_impl *cache=&storage->state;bool found=false;
    for(unsigned i=0;i<cache->instance_count;){
        ds_cache_instance_entry *instance=&cache->instances[i];
        if(instance->pending_tx!=ticket.value){i++;continue;}
        found=true;
        if(instance->flags&INSTANCE_PENDING_NEW){
            memmove(instance,instance+1,(cache->instance_count-i-1)*sizeof(*instance));
            cache->instance_count--;continue;
        }
        instance->pending_tx=0;i++;
    }
    return found?DS_OK:DS_STALE;
}
ds_result ds_cache_resolve(ds_cache *storage,const ds_core *core,ds_tx ticket,bool presented){
    if(!storage||!core||!ticket.value)return DS_INVALID;
    if(ds_core_has_submission(core))return DS_BUSY;
    ds_cache_impl *cache=&storage->state;bool found=false,pruned=false;
    for(unsigned i=0;i<cache->instance_count;){
        ds_cache_instance_entry *instance=&cache->instances[i];
        if(instance->pending_tx==ticket.value){
            found=true;
            if(!presented&&(instance->flags&INSTANCE_PENDING_NEW)){
                memmove(instance,instance+1,(cache->instance_count-i-1)*sizeof(*instance));cache->instance_count--;continue;
            }
            if(presented)instance->current=instance->pending;
            instance->pending_tx=0;instance->flags&=(uint8_t)~INSTANCE_PENDING_NEW;
        }
        if(presented&&!ds_core_refs_active(core,(ds_layer)instance->layer,instance->first,instance->command_count)){
            memmove(instance,instance+1,(cache->instance_count-i-1)*sizeof(*instance));
            cache->instance_count--;pruned=true;continue;
        }
        i++;
    }
    return found||pruned?DS_OK:DS_STALE;
}
ds_cache_stats ds_cache_get_stats(const ds_cache *storage){
    if(!storage)return (ds_cache_stats){0};
    const ds_cache_impl *cache=&storage->state;
    return (ds_cache_stats){cache->command_used,cache->text_used,cache->template_count,cache->instance_count,
                            sizeof(ds_cache)+sizeof(last_template)+sizeof(last_instance)};
}
