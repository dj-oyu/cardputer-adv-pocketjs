#include "ds_core.h"
#include <string.h>

#define DS_REF_INDEX_BITS 7u
#define DS_REF_INDEX_MASK ((1u << DS_REF_INDEX_BITS) - 1u)
#define DS_REF_GENERATION_MAX ((1u << (32u - DS_REF_INDEX_BITS)) - 1u)
#define DS_FLAG_VISIBLE 1u

/* Process-lifetime IDs; all cores use the same owner task. Never reset these
 * with a guest session. Exhaustion fails closed rather than reviving handles. */
static uint32_t last_generation,last_transaction,last_resource;

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

static ds_core_impl *impl(ds_core *core){return &core->state;}
static const ds_core_impl *cimpl(const ds_core *core){return &core->state;}
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
static ds_result check_transaction(void *context,ds_tx tx){
    ds_endpoint *endpoint=context;ds_core_impl *core=endpoint->core;
    if(!core->building||endpoint->layer!=core->layer||tx.value==0||tx.value!=core->transaction.value)return DS_STALE;
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
static const ds_image_entry *find_image(const ds_core_impl *core,ds_layer layer,ds_resource id){
    for(unsigned i=0;i<core->image_count;i++)
        if(core->images[i].id.value==id.value&&core->images[i].layer==layer)return &core->images[i];
    return NULL;
}
static ds_result validate_image(const ds_core_impl *core,ds_layer layer,ds_resource id,uint16_t variant,uint16_t frame){
    const ds_image_entry *entry=find_image(core,layer,id);
    if(!entry)return DS_STALE;
    return variant<entry->port.variants&&frame<entry->port.frames?DS_OK:DS_INVALID;
}

static ds_result core_begin(void *context,ds_update_mode mode,ds_tx *out){
    ds_endpoint *endpoint=context;ds_core_impl *core=endpoint->core;
    if(!out||(mode!=DS_REPLACE&&mode!=DS_PATCH))return DS_INVALID;
    if(core->building||core->submitted)return DS_BUSY;
    ds_layer layer=endpoint->layer;
    if(last_transaction==UINT32_MAX||
       (mode==DS_REPLACE&&last_generation==DS_REF_GENERATION_MAX))return DS_LIMIT;
    core->building_bank=(uint8_t)(core->active^1u);
    memcpy(&core->banks[core->building_bank],&core->banks[core->active],sizeof(ds_bank));
    core->layer=layer;core->mode=mode;core->poison=DS_OK;core->building=true;
    core->transaction=(ds_tx){++last_transaction};*out=core->transaction;
    if(mode==DS_REPLACE){
        uint32_t generation=++last_generation;
        ds_bank *bank=&core->banks[core->building_bank];
        bank->generation[layer]=generation;bank->count[layer]=0;bank->text_used[layer]=0;
        bank->background_set[layer]=false;
        memset(bank->commands+command_base(layer),0,command_limit(layer)*sizeof(ds_command_storage));
        memset(bank->text+text_base(layer),0,text_limit(layer));
    }
    return DS_OK;
}
static ds_result core_background(void *context,ds_tx tx,ds_rgba color){
    ds_core_impl *core=((ds_endpoint *)context)->core;ds_result result=check_transaction(context,tx);
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
        if((unsigned)draw->data.text.font>DS_DISPLAY)return DS_INVALID;
        return utf8_count(draw->data.text.utf8,draw->data.text.bytes)==SIZE_MAX?DS_INVALID:DS_OK;
    case DS_IMAGE:return draw->data.image.resource.value?DS_OK:DS_INVALID;
    }
    return DS_INVALID;
}
static ds_result core_add(void *context,ds_tx tx,const ds_draw *draw,ds_ref *out){
    ds_core_impl *core=((ds_endpoint *)context)->core;ds_result result=check_transaction(context,tx);
    if(result!=DS_OK)return result;
    if(core->mode!=DS_REPLACE)return poison(core,DS_INVALID);
    if(!out)return poison(core,DS_INVALID);
    result=validate_draw(draw);if(result!=DS_OK)return poison(core,result);
    if(draw->kind==DS_IMAGE){
        result=validate_image(core,core->layer,draw->data.image.resource,draw->data.image.variant,draw->data.image.frame);
        if(result!=DS_OK)return poison(core,result);
    }
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
        memset(bank->text+offset,0,capacity);
        if(draw->data.text.bytes)memcpy(bank->text+offset,draw->data.text.utf8,draw->data.text.bytes);
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
    ds_core_impl *core=((ds_endpoint *)context)->core;ds_result result=check_transaction(context,tx);
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
        if(change->value.text.bytes>p.capacity)return poison(core,DS_LIMIT);
        size_t count=utf8_count(change->value.text.utf8,change->value.text.bytes);
        if(count==SIZE_MAX)return poison(core,DS_INVALID);
        ds_bank *bank=&core->banks[core->building_bank];
        memset(bank->text+p.offset,0,p.capacity);
        if(change->value.text.bytes)memcpy(bank->text+p.offset,change->value.text.utf8,change->value.text.bytes);
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
        image_payload p;payload_read(command,&p,sizeof(p));
        result=validate_image(core,core->layer,(ds_resource){p.resource},change->value.image.variant,change->value.image.frame);
        if(result!=DS_OK)return poison(core,result);
        p.variant=change->value.image.variant;p.frame=change->value.image.frame;
        payload_write(command,&p,sizeof(p));return DS_OK;
    }
    }
    return poison(core,DS_INVALID);
}
static ds_result core_animate(void *context,ds_tx tx,const ds_motion *motion,ds_animation *out){
    ds_core_impl *core=((ds_endpoint *)context)->core;ds_result result=check_transaction(context,tx);
    if(result!=DS_OK)return result;
    (void)motion;(void)out;return poison(core,DS_UNSUPPORTED);
}
static ds_result core_stop(void *context,ds_tx tx,ds_animation animation){
    ds_core_impl *core=((ds_endpoint *)context)->core;ds_result result=check_transaction(context,tx);
    if(result!=DS_OK)return result;
    (void)animation;return poison(core,DS_UNSUPPORTED);
}
static ds_result core_end(void *context,ds_tx tx){
    ds_core_impl *core=((ds_endpoint *)context)->core;ds_result result=check_transaction(context,tx);
    if(result!=DS_OK)return result;
    if(!core->banks[core->building_bank].background_set[DS_APP])return poison(core,DS_INVALID);
    core->building=false;core->submitted=true;return DS_OK;
}
static void core_abort(void *context,ds_tx tx){
    ds_core_impl *core=((ds_endpoint *)context)->core;
    if(core->building&&((ds_endpoint *)context)->layer==core->layer&&
       tx.value==core->transaction.value)core->building=false;
}
static ds_limits core_limits(void *context){
    (void)context;return (ds_limits){{DS_APP_COMMANDS,DS_APP_TEXT_BYTES,0},
                                    {DS_SYSTEM_COMMANDS,DS_SYSTEM_TEXT_BYTES,0},
                                    sizeof(ds_core)+sizeof(last_generation)+sizeof(last_transaction)+sizeof(last_resource),0};
}
static ds_stats core_stats(void *context){
    ds_core_impl *core=((ds_endpoint *)context)->core;const ds_bank *bank=&core->banks[core->active];
    ds_stats stats={0};
    for(unsigned layer=0;layer<2;layer++)stats.used[layer]=(ds_capacity){bank->count[layer],bank->text_used[layer],0};
    stats.native_current=sizeof(ds_core)+sizeof(last_generation)+sizeof(last_transaction)+sizeof(last_resource);
    stats.native_peak=stats.native_current;return stats;
}
static const ds_api core_api={core_begin,core_background,core_add,core_change,core_animate,
                              core_stop,core_end,core_abort,core_limits,core_stats};

void ds_core_init(ds_core *storage){
    if(!storage)return;
    memset(storage,0,sizeof(*storage));ds_core_impl *core=impl(storage);
    for(unsigned layer=0;layer<2;layer++){
        core->endpoints[layer]=(ds_endpoint){core,(ds_layer)layer};
    }
    /* SYSTEM can paint before the first APP submission. APP REPLACE still
     * requires an explicit opaque background. */
    core->banks[0].background[DS_APP]=0x000000ff;
    core->banks[0].background_set[DS_APP]=true;
    core->full_redraw=true;
}
ds_client ds_core_client(ds_core *storage,ds_layer layer){
    if(!storage||!valid_layer(layer))return (ds_client){0};
    ds_core_impl *core=impl(storage);return (ds_client){&core_api,&core->endpoints[layer]};
}
ds_result ds_core_register_image(ds_core *storage,ds_layer layer,const ds_image_port *port,ds_resource *out){
    if(!storage||!valid_layer(layer)||!port||!out||!port->read_span||
       !port->width||!port->height||!port->variants||!port->frames)return DS_INVALID;
    ds_core_impl *core=impl(storage);
    if(core->building||core->submitted)return DS_BUSY;
    if(core->image_count==DS_RESOURCES||last_resource==UINT32_MAX)return DS_LIMIT;
    ds_image_entry *entry=&core->images[core->image_count++];
    *entry=(ds_image_entry){*port,{++last_resource},layer};*out=entry->id;return DS_OK;
}
bool ds_core_has_submission(const ds_core *storage){return storage&&cimpl(storage)->submitted;}
ds_result ds_core_presented(ds_core *storage,ds_tx ticket){
    if(!storage)return DS_INVALID;
    ds_core_impl *core=impl(storage);
    if(!core->submitted||ticket.value!=core->transaction.value)return DS_STALE;
    core->active=core->building_bank;core->submitted=false;core->full_redraw=false;return DS_OK;
}
ds_result ds_core_discard(ds_core *storage,ds_tx ticket){
    if(!storage)return DS_INVALID;
    ds_core_impl *core=impl(storage);
    if(!core->submitted||ticket.value!=core->transaction.value)return DS_STALE;
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

ds_result ds_core_frame(const ds_core *storage,ds_frame *out){
    if(!storage||!out)return DS_INVALID;
    const ds_core_impl *core=cimpl(storage);
    if(!core->submitted)return DS_STALE;
    const ds_bank *before=&core->banks[core->active],*after=&core->banks[core->building_bank];
    *out=(ds_frame){.ticket=core->transaction,.previous_background=before->background[DS_APP],
                    .next_background=after->background[DS_APP],.full_redraw=core->full_redraw};
    for(unsigned i=0;i<2;i++){
        out->previous[i]=usage(before,(ds_layer)i);out->next[i]=usage(after,(ds_layer)i);
    }
    return DS_OK;
}
ds_result ds_core_failed(ds_core *storage,ds_tx ticket){
    if(!storage)return DS_INVALID;
    ds_core_impl *core=impl(storage);
    if(!core->submitted||ticket.value!=core->transaction.value)return DS_STALE;
    core->full_redraw=true;return DS_OK;
}
ds_result ds_core_read(const ds_core *storage,ds_tx ticket,bool previous,
                       ds_layer layer,uint16_t index,ds_frame_command *out){
    if(!storage||!out||!valid_layer(layer))return DS_INVALID;
    const ds_core_impl *core=cimpl(storage);
    if(!core->submitted||ticket.value!=core->transaction.value)return DS_STALE;
    const ds_bank *bank=&core->banks[previous?core->active:core->building_bank];
    if(index>=bank->count[layer])return DS_INVALID;
    const ds_command_storage *command=&bank->commands[command_base(layer)+index];
    memset(out,0,sizeof(*out));
    ds_draw *draw=&out->draw;
    draw->kind=(ds_kind)command->kind;draw->bounds=command->bounds;
    draw->clip=command->clip;draw->opacity=command->opacity;
    out->visible=(command->flags&DS_FLAG_VISIBLE)!=0;
    switch(draw->kind){
    case DS_RECT:case DS_ROUND_RECT:case DS_STROKE:{
        shape_payload p;payload_read(command,&p,sizeof(p));
        draw->data.shape.color=p.color;draw->data.shape.radius=p.radius;draw->data.shape.width=p.width;break;
    }
    case DS_GRADIENT:{
        gradient_payload p;payload_read(command,&p,sizeof(p));
        draw->data.gradient.from=p.from;draw->data.gradient.to=p.to;
        draw->data.gradient.axis=p.axis;draw->data.gradient.radius=p.radius;
        draw->data.gradient.dither=p.dither!=0;break;
    }
    case DS_TEXT:{
        text_payload p;payload_read(command,&p,sizeof(p));
        memcpy(out->text,bank->text+p.offset,p.length);
        draw->data.text.utf8=out->text;draw->data.text.bytes=p.length;
        draw->data.text.capacity=p.capacity;draw->data.text.font=(ds_font)p.font;
        draw->data.text.color=p.color;out->reveal=p.reveal;break;
    }
    case DS_IMAGE:{
        image_payload p;payload_read(command,&p,sizeof(p));
        draw->data.image.resource=(ds_resource){p.resource};
        draw->data.image.variant=p.variant;draw->data.image.frame=p.frame;break;
    }
    default:return DS_INVALID;
    }
    return DS_OK;
}

ds_result ds_core_image_span(const ds_core *storage,ds_tx ticket,bool previous,
                            ds_layer layer,uint16_t index,uint16_t y,uint16_t x,
                            uint16_t count,uint16_t *rgb565,uint8_t *alpha){
    if(!storage||!valid_layer(layer))return DS_INVALID;
    const ds_core_impl *core=cimpl(storage);
    if(!core->submitted||ticket.value!=core->transaction.value)return DS_STALE;
    const ds_bank *bank=&core->banks[previous?core->active:core->building_bank];
    if(index>=bank->count[layer])return DS_INVALID;
    const ds_command_storage *command=&bank->commands[command_base(layer)+index];
    if(command->kind!=DS_IMAGE)return DS_INVALID;
    image_payload p;payload_read(command,&p,sizeof(p));
    const ds_image_entry *entry=find_image(core,layer,(ds_resource){p.resource});
    if(!entry)return DS_STALE;
    if(y>=entry->port.height||x>entry->port.width||count>entry->port.width-x||
       (count&&(!rgb565||!alpha)))return DS_INVALID;
    if(!count)return DS_OK;
    return entry->port.read_span(entry->port.ctx,p.variant,p.frame,y,x,count,rgb565,alpha);
}

static uint32_t command_bands(const ds_command_storage *command){
    if(!(command->flags&DS_FLAG_VISIBLE)||!command->opacity)return 0;
    int32_t x0=command->bounds.x0,x1=command->bounds.x1,y0=command->bounds.y0,y1=command->bounds.y1;
    if(x0<command->clip.x0)x0=command->clip.x0;
    if(x1>command->clip.x1)x1=command->clip.x1;
    if(y0<command->clip.y0)y0=command->clip.y0;
    if(y1>command->clip.y1)y1=command->clip.y1;
    if(x0<0)x0=0;
    if(x1>240)x1=240;
    if(y0<0)y0=0;
    if(y1>135)y1=135;
    if(x0>=x1||y0>=y1)return 0;
    uint32_t mask=0;
    for(int32_t band=y0/8;band<=(y1-1)/8;band++)mask|=1u<<band;
    return mask;
}
ds_result ds_core_damage(const ds_core *storage,ds_tx ticket,uint32_t *bands){
    if(!storage||!bands)return DS_INVALID;
    const ds_core_impl *core=cimpl(storage);
    if(!core->submitted||ticket.value!=core->transaction.value)return DS_STALE;
    const ds_bank *old=&core->banks[core->active],*next=&core->banks[core->building_bank];
    *bands=0;
    if(core->full_redraw||old->background[DS_APP]!=next->background[DS_APP]||
       old->generation[0]!=next->generation[0]||old->generation[1]!=next->generation[1]){
        *bands=(1u<<17)-1u;return DS_OK;
    }
    for(unsigned layer=0;layer<2;layer++)for(unsigned i=0;i<next->count[layer];i++){
        unsigned index=command_base((ds_layer)layer)+i;
        const ds_command_storage *a=&old->commands[index],*b=&next->commands[index];
        bool changed=memcmp(a,b,sizeof(*a))!=0;
        if(!changed&&b->kind==DS_TEXT){
            text_payload p;payload_read(b,&p,sizeof(p));
            changed=memcmp(old->text+p.offset,next->text+p.offset,p.length)!=0;
        }
        if(changed)*bands|=command_bands(a)|command_bands(b);
    }
    return DS_OK;
}
