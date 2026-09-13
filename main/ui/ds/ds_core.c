#include "ds_core.h"
#include <string.h>

#define DS_REF_INDEX_BITS 7u
#define DS_REF_INDEX_MASK ((1u << DS_REF_INDEX_BITS) - 1u)
#define DS_REF_GENERATION_MAX ((1u << (32u - DS_REF_INDEX_BITS)) - 1u)
#define DS_FLAG_VISIBLE 1u

typedef struct ds_core_impl ds_core_impl;
typedef struct { ds_core_impl *core; ds_layer layer; } ds_endpoint;
typedef struct {
    ds_command_storage commands[DS_COMMANDS];
    uint8_t text[DS_TEXT_BYTES];
    uint16_t count[2],text_used[2];
    uint32_t generation[2];
    ds_rgba background[2];
    bool background_set[2];
} ds_bank;
struct ds_core_impl {
    ds_bank banks[2];
    ds_endpoint endpoints[2];
    uint32_t next_generation[2],next_tx;
    ds_tx transaction;
    ds_result poison;
    ds_layer layer;
    ds_update_mode mode;
    uint8_t active,building_bank;
    bool building,submitted,generation_exhausted[2];
};

typedef struct { ds_rgba color; uint8_t radius,width,pad[2]; } shape_payload;
typedef struct { ds_rgba from,to; uint8_t axis,radius,dither,pad; } gradient_payload;
typedef struct {
    uint16_t offset;
    uint8_t length,capacity,font,reveal;
    uint16_t flags;
    ds_rgba color;
} text_payload;
typedef struct { uint32_t resource; uint16_t variant,frame; uint32_t pad; } image_payload;

_Static_assert(sizeof(ds_core_impl)<=DS_CORE_STORAGE_BYTES,"core storage budget");
_Static_assert(sizeof(shape_payload)<=sizeof(((ds_command_storage *)0)->payload),"shape payload");
_Static_assert(sizeof(gradient_payload)<=sizeof(((ds_command_storage *)0)->payload),"gradient payload");
_Static_assert(sizeof(text_payload)<=sizeof(((ds_command_storage *)0)->payload),"text payload");
_Static_assert(sizeof(image_payload)<=sizeof(((ds_command_storage *)0)->payload),"image payload");

static ds_core_impl *impl(ds_core *core){return (ds_core_impl *)(void *)core->bytes;}
static const ds_core_impl *cimpl(const ds_core *core){return (const ds_core_impl *)(const void *)core->bytes;}
static unsigned command_base(ds_layer layer){return layer==DS_APP?0u:DS_APP_COMMANDS;}
static unsigned command_limit(ds_layer layer){return layer==DS_APP?DS_APP_COMMANDS:DS_SYSTEM_COMMANDS;}
static unsigned text_base(ds_layer layer){return layer==DS_APP?0u:DS_APP_TEXT_BYTES;}
static unsigned text_limit(ds_layer layer){return layer==DS_APP?DS_APP_TEXT_BYTES:DS_SYSTEM_TEXT_BYTES;}
static bool valid_layer(ds_layer layer){return layer==DS_APP||layer==DS_SYSTEM;}
static bool valid_rect(ds_rect r){return r.x0<=r.x1&&r.y0<=r.y1;}
static unsigned rect_width(ds_rect r){return (unsigned)((int32_t)r.x1-r.x0);}
static unsigned rect_height(ds_rect r){return (unsigned)((int32_t)r.y1-r.y0);}
static bool valid_radius(ds_rect r,uint8_t radius){
    return radius<=8u&&radius<=rect_width(r)/2u&&radius<=rect_height(r)/2u;
}
static ds_ref make_ref(uint32_t generation,unsigned index){
    return (ds_ref){(generation<<DS_REF_INDEX_BITS)|(uint32_t)index};
}
static uint32_t ref_generation(ds_ref ref){return ref.value>>DS_REF_INDEX_BITS;}
static unsigned ref_index(ds_ref ref){return ref.value&DS_REF_INDEX_MASK;}
static ds_result poison(ds_core_impl *core,ds_result result){
    if(core->poison==DS_OK)core->poison=result;
    return result;
}
static ds_result check_transaction(ds_core_impl *core,ds_tx tx){
    if(!core->building||tx.value==0||tx.value!=core->transaction.value)return DS_STALE;
    return core->poison;
}

/* Returns code-point count, or SIZE_MAX for malformed/unsupported text. */
static size_t utf8_count(const char *text,size_t bytes){
    if(bytes&&!text)return SIZE_MAX;
    size_t i=0,count=0;
    while(i<bytes){
        uint8_t a=(uint8_t)text[i++];uint32_t cp;unsigned more;
        if(a<0x80u){cp=a;more=0;}
        else if(a>=0xc2u&&a<=0xdfu){cp=a&0x1fu;more=1;}
        else if(a>=0xe0u&&a<=0xefu){cp=a&0x0fu;more=2;}
        else if(a>=0xf0u&&a<=0xf4u){cp=a&7u;more=3;}
        else return SIZE_MAX;
        if(i+more>bytes)return SIZE_MAX;
        for(unsigned n=0;n<more;n++){
            uint8_t b=(uint8_t)text[i++];if((b&0xc0u)!=0x80u)return SIZE_MAX;
            cp=(cp<<6)|(b&0x3fu);
        }
        if((more==2&&(cp<0x800u||(cp>=0xd800u&&cp<=0xdfffu)))||
           (more==3&&(cp<0x10000u||cp>0x10ffffu)))return SIZE_MAX;
        if(cp<0x20u||cp==0x7fu)return SIZE_MAX;
        count++;
    }
    return count;
}
static void payload_write(ds_command_storage *command,const void *payload,size_t bytes){
    memset(command->payload,0,sizeof(command->payload));memcpy(command->payload,payload,bytes);
}
static void payload_read(const ds_command_storage *command,void *payload,size_t bytes){
    memcpy(payload,command->payload,bytes);
}

static ds_result core_begin(void *context,ds_update_mode mode,ds_tx *out){
    ds_endpoint *endpoint=context;ds_core_impl *core=endpoint->core;
    if(!out||(mode!=DS_REPLACE&&mode!=DS_PATCH))return DS_INVALID;
    if(core->building||core->submitted)return DS_BUSY;
    ds_layer layer=endpoint->layer;
    if(mode==DS_REPLACE&&core->generation_exhausted[layer])return DS_LIMIT;
    core->building_bank=(uint8_t)(core->active^1u);
    memcpy(&core->banks[core->building_bank],&core->banks[core->active],sizeof(ds_bank));
    core->layer=layer;core->mode=mode;core->poison=DS_OK;core->building=true;
    if(++core->next_tx==0)++core->next_tx;
    core->transaction=(ds_tx){core->next_tx};*out=core->transaction;
    if(mode==DS_REPLACE){
        uint32_t generation=core->next_generation[layer];
        if(generation==DS_REF_GENERATION_MAX)core->generation_exhausted[layer]=true;
        else core->next_generation[layer]=generation+1u;
        ds_bank *bank=&core->banks[core->building_bank];
        bank->generation[layer]=generation;bank->count[layer]=0;bank->text_used[layer]=0;
        bank->background_set[layer]=false;
        memset(bank->commands+command_base(layer),0,command_limit(layer)*sizeof(ds_command_storage));
        memset(bank->text+text_base(layer),0,text_limit(layer));
    }
    return DS_OK;
}
static ds_result core_background(void *context,ds_tx tx,ds_rgba color){
    ds_core_impl *core=((ds_endpoint *)context)->core;ds_result result=check_transaction(core,tx);
    if(result!=DS_OK)return result;
    if(core->layer!=DS_APP||(color&0xffu)!=0xffu)return poison(core,DS_INVALID);
    ds_bank *bank=&core->banks[core->building_bank];bank->background[DS_APP]=color;
    bank->background_set[DS_APP]=true;return DS_OK;
}
static ds_result validate_draw(const ds_draw *draw){
    if(!draw||draw->kind<DS_RECT||draw->kind>DS_IMAGE||!valid_rect(draw->bounds)||!valid_rect(draw->clip))return DS_INVALID;
    switch(draw->kind){
    case DS_RECT:return draw->data.shape.radius==0&&draw->data.shape.width==0?DS_OK:DS_INVALID;
    case DS_ROUND_RECT:return draw->data.shape.width==0&&valid_radius(draw->bounds,draw->data.shape.radius)?DS_OK:DS_INVALID;
    case DS_STROKE:return (draw->data.shape.width==1||draw->data.shape.width==2)&&draw->data.shape.radius==0?DS_OK:DS_INVALID;
    case DS_GRADIENT:
        return draw->data.gradient.axis<=1&&valid_radius(draw->bounds,draw->data.gradient.radius)?DS_OK:DS_INVALID;
    case DS_TEXT:
        if(draw->data.text.capacity<1||draw->data.text.capacity>128)return DS_LIMIT;
        if(draw->data.text.bytes>draw->data.text.capacity)return DS_LIMIT;
        if(draw->data.text.font>DS_DISPLAY)return DS_INVALID;
        return utf8_count(draw->data.text.utf8,draw->data.text.bytes)==SIZE_MAX?DS_INVALID:DS_OK;
    case DS_IMAGE:return draw->data.image.resource.value?DS_OK:DS_INVALID;
    }
    return DS_INVALID;
}
static ds_result core_add(void *context,ds_tx tx,const ds_draw *draw,ds_ref *out){
    ds_core_impl *core=((ds_endpoint *)context)->core;ds_result result=check_transaction(core,tx);
    if(result!=DS_OK)return result;
    if(!out)return poison(core,DS_INVALID);
    result=validate_draw(draw);if(result!=DS_OK)return poison(core,result);
    ds_bank *bank=&core->banks[core->building_bank];ds_layer layer=core->layer;
    if(bank->count[layer]>=command_limit(layer))return poison(core,DS_LIMIT);
    unsigned index=command_base(layer)+bank->count[layer];
    ds_command_storage command={.kind=(uint8_t)draw->kind,.flags=DS_FLAG_VISIBLE,
                                .opacity=draw->opacity,.bounds=draw->bounds,.clip=draw->clip};
    switch(draw->kind){
    case DS_RECT:case DS_ROUND_RECT:case DS_STROKE:{
        shape_payload payload={draw->data.shape.color,draw->data.shape.radius,draw->data.shape.width,{0,0}};
        payload_write(&command,&payload,sizeof(payload));break;
    }
    case DS_GRADIENT:{
        gradient_payload payload={draw->data.gradient.from,draw->data.gradient.to,
                                  draw->data.gradient.axis,draw->data.gradient.radius,
                                  draw->data.gradient.dither,0};
        payload_write(&command,&payload,sizeof(payload));break;
    }
    case DS_TEXT:{
        unsigned used=bank->text_used[layer],capacity=draw->data.text.capacity;
        if(used+capacity>text_limit(layer))return poison(core,DS_LIMIT);
        unsigned offset=text_base(layer)+used;
        memset(bank->text+offset,0,capacity);memcpy(bank->text+offset,draw->data.text.utf8,draw->data.text.bytes);
        text_payload payload={(uint16_t)offset,(uint8_t)draw->data.text.bytes,(uint8_t)capacity,
                              (uint8_t)draw->data.text.font,(uint8_t)utf8_count(draw->data.text.utf8,draw->data.text.bytes),
                              0,draw->data.text.color};
        payload_write(&command,&payload,sizeof(payload));bank->text_used[layer]=(uint16_t)(used+capacity);break;
    }
    case DS_IMAGE:{
        image_payload payload={draw->data.image.resource.value,draw->data.image.variant,draw->data.image.frame,0};
        payload_write(&command,&payload,sizeof(payload));break;
    }
    }
    bank->commands[index]=command;bank->count[layer]++;
    *out=make_ref(bank->generation[layer],index);return DS_OK;
}
static ds_result referenced_command(ds_core_impl *core,ds_ref ref,ds_command_storage **out){
    unsigned index=ref_index(ref),base=command_base(core->layer),limit=command_limit(core->layer);
    ds_bank *bank=&core->banks[core->building_bank];
    if(index<base||index>=base+limit||index>=base+bank->count[core->layer]||
       ref_generation(ref)!=bank->generation[core->layer])return DS_STALE;
    *out=&bank->commands[index];return DS_OK;
}
static ds_result core_change(void *context,ds_tx tx,ds_ref ref,const ds_change *change){
    ds_core_impl *core=((ds_endpoint *)context)->core;ds_result result=check_transaction(core,tx);
    if(result!=DS_OK)return result;
    if(!change)return poison(core,DS_INVALID);
    ds_command_storage *command;result=referenced_command(core,ref,&command);
    if(result!=DS_OK)return poison(core,result);
    switch(change->property){
    case DS_SET_RECT:
        if(!valid_rect(change->value.rect))return poison(core,DS_INVALID);
        if(command->kind==DS_ROUND_RECT){shape_payload p;payload_read(command,&p,sizeof(p));
            if(!valid_radius(change->value.rect,p.radius))return poison(core,DS_INVALID);}
        if(command->kind==DS_GRADIENT){gradient_payload p;payload_read(command,&p,sizeof(p));
            if(!valid_radius(change->value.rect,p.radius))return poison(core,DS_INVALID);}
        command->bounds=change->value.rect;return DS_OK;
    case DS_SET_CLIP:
        if(!valid_rect(change->value.rect))return poison(core,DS_INVALID);
        command->clip=change->value.rect;return DS_OK;
    case DS_SET_COLOR:
        if(command->kind==DS_RECT||command->kind==DS_ROUND_RECT||command->kind==DS_STROKE){
            shape_payload p;payload_read(command,&p,sizeof(p));p.color=change->value.color;payload_write(command,&p,sizeof(p));return DS_OK;
        }
        if(command->kind==DS_TEXT){text_payload p;payload_read(command,&p,sizeof(p));p.color=change->value.color;payload_write(command,&p,sizeof(p));return DS_OK;}
        return poison(core,DS_UNSUPPORTED);
    case DS_SET_TEXT:{
        if(command->kind!=DS_TEXT)return poison(core,DS_INVALID);
        text_payload p;payload_read(command,&p,sizeof(p));
        size_t count=utf8_count(change->value.text.utf8,change->value.text.bytes);
        if(count==SIZE_MAX)return poison(core,DS_INVALID);
        if(change->value.text.bytes>p.capacity)return poison(core,DS_LIMIT);
        ds_bank *bank=&core->banks[core->building_bank];
        memset(bank->text+p.offset,0,p.capacity);memcpy(bank->text+p.offset,change->value.text.utf8,change->value.text.bytes);
        p.length=(uint8_t)change->value.text.bytes;p.reveal=(uint8_t)count;payload_write(command,&p,sizeof(p));return DS_OK;
    }
    case DS_SET_REVEAL:{
        if(command->kind!=DS_TEXT)return poison(core,DS_INVALID);
        text_payload p;payload_read(command,&p,sizeof(p));ds_bank *bank=&core->banks[core->building_bank];
        size_t count=utf8_count((const char *)bank->text+p.offset,p.length);
        if(change->value.reveal>count)return poison(core,DS_INVALID);
        p.reveal=(uint8_t)change->value.reveal;payload_write(command,&p,sizeof(p));return DS_OK;
    }
    case DS_SET_VISIBLE:
        if(change->value.visible)command->flags|=DS_FLAG_VISIBLE;else command->flags&=(uint8_t)~DS_FLAG_VISIBLE;
        return DS_OK;
    case DS_SET_IMAGE_FRAME:{
        if(command->kind!=DS_IMAGE)return poison(core,DS_INVALID);
        image_payload p;payload_read(command,&p,sizeof(p));p.variant=change->value.image.variant;p.frame=change->value.image.frame;
        payload_write(command,&p,sizeof(p));return DS_OK;
    }
    }
    return poison(core,DS_INVALID);
}
static ds_result core_animate(void *context,ds_tx tx,const ds_motion *motion,ds_animation *out){
    ds_core_impl *core=((ds_endpoint *)context)->core;ds_result result=check_transaction(core,tx);
    if(result!=DS_OK)return result;
    (void)motion;(void)out;return poison(core,DS_UNSUPPORTED);
}
static ds_result core_stop(void *context,ds_tx tx,ds_animation animation){
    ds_core_impl *core=((ds_endpoint *)context)->core;ds_result result=check_transaction(core,tx);
    if(result!=DS_OK)return result;
    (void)animation;return poison(core,DS_UNSUPPORTED);
}
static ds_result core_end(void *context,ds_tx tx){
    ds_core_impl *core=((ds_endpoint *)context)->core;ds_result result=check_transaction(core,tx);
    if(result!=DS_OK)return result;
    if(!core->banks[core->building_bank].background_set[DS_APP])return poison(core,DS_INVALID);
    core->building=false;core->submitted=true;return DS_OK;
}
static void core_abort(void *context,ds_tx tx){
    ds_core_impl *core=((ds_endpoint *)context)->core;
    if(core->building&&tx.value==core->transaction.value)core->building=false;
}
static ds_limits core_limits(void *context){
    (void)context;return (ds_limits){{DS_APP_COMMANDS,DS_APP_TEXT_BYTES,6},
                                    {DS_SYSTEM_COMMANDS,DS_SYSTEM_TEXT_BYTES,2},
                                    sizeof(ds_core),0};
}
static ds_stats core_stats(void *context){
    ds_core_impl *core=((ds_endpoint *)context)->core;const ds_bank *bank=&core->banks[core->active];
    ds_stats stats={0};
    for(unsigned layer=0;layer<2;layer++)stats.used[layer]=(ds_capacity){bank->count[layer],bank->text_used[layer],0};
    stats.native_current=sizeof(ds_core);stats.native_peak=sizeof(ds_core);return stats;
}
static const ds_api core_api={core_begin,core_background,core_add,core_change,core_animate,
                              core_stop,core_end,core_abort,core_limits,core_stats};

void ds_core_init(ds_core *storage){
    if(!storage)return;
    memset(storage,0,sizeof(*storage));ds_core_impl *core=impl(storage);
    for(unsigned layer=0;layer<2;layer++){
        core->endpoints[layer]=(ds_endpoint){core,(ds_layer)layer};core->next_generation[layer]=2;
        core->banks[0].generation[layer]=1;core->banks[1].generation[layer]=1;
    }
}
ds_client ds_core_client(ds_core *storage,ds_layer layer){
    if(!storage||!valid_layer(layer))return (ds_client){0};
    ds_core_impl *core=impl(storage);return (ds_client){&core_api,&core->endpoints[layer]};
}
bool ds_core_has_submission(const ds_core *storage){return storage&&cimpl(storage)->submitted;}
ds_result ds_core_presented(ds_core *storage){
    if(!storage)return DS_INVALID;
    ds_core_impl *core=impl(storage);
    if(!core->submitted)return DS_STALE;
    core->active=core->building_bank;core->submitted=false;return DS_OK;
}
ds_result ds_core_discard(ds_core *storage){
    if(!storage)return DS_INVALID;
    ds_core_impl *core=impl(storage);
    if(!core->submitted)return DS_STALE;
    core->submitted=false;return DS_OK;
}
static ds_capacity usage(const ds_bank *bank,ds_layer layer){
    if(!valid_layer(layer))return (ds_capacity){0};
    return (ds_capacity){bank->count[layer],bank->text_used[layer],0};
}
ds_capacity ds_core_active_usage(const ds_core *storage,ds_layer layer){
    if(!storage)return (ds_capacity){0};
    const ds_core_impl *core=cimpl(storage);return usage(&core->banks[core->active],layer);
}
ds_capacity ds_core_submission_usage(const ds_core *storage,ds_layer layer){
    if(!storage)return (ds_capacity){0};
    const ds_core_impl *core=cimpl(storage);
    return core->submitted?usage(&core->banks[core->building_bank],layer):(ds_capacity){0};
}
