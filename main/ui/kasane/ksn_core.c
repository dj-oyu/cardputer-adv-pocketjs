#include "ksn_core.h"
#include "ksn_image_transform.h"
#include <string.h>

#define KSN_REF_INDEX_BITS 7u
#define KSN_REF_INDEX_MASK ((1u << KSN_REF_INDEX_BITS) - 1u)
#define KSN_REF_GENERATION_MAX ((1u << (32u - KSN_REF_INDEX_BITS)) - 1u)
#define KSN_FLAG_VISIBLE 1u
#define KSN_FLAG_GROUP 2u
#define KSN_FLAG_GROUP_BEGIN 4u
#define KSN_FLAG_GROUP_END 8u
#define KSN_IMAGE_SCALE_SHIFT 4u

/* Process-lifetime IDs; all cores use the same owner task. Never reset these
 * with a guest session. Exhaustion fails closed rather than reviving handles. */
static uint32_t last_generation,last_transaction,last_resource,last_animation;
_Static_assert(sizeof(ksn_track)<=64,"animation track budget");
_Static_assert(2*sizeof(ksn_core_animation_block)<=1024,"animation bank budget");

typedef struct { ksn_rgba color; uint8_t radius,width,pad[2]; } shape_payload;
typedef struct { ksn_rgba from,to; uint8_t axis,radius,dither,pad; } gradient_payload;
typedef struct {
    uint16_t offset;
    uint8_t length,capacity,font,reveal;
    uint16_t flags;
    ksn_rgba color;
} text_payload;
typedef struct { uint32_t resource; uint16_t variant,frame,source_x,source_y; } image_payload;
typedef struct { uint32_t resource; uint8_t variant,frame; uint16_t rotation; uint8_t x,y,w,h; } stretch_payload;

_Static_assert(KSN_CORE_RESERVED_BYTES<=KSN_CORE_STORAGE_BYTES,"core storage budget");
_Static_assert(sizeof(ksn_core)<=3072,"core control allocation budget");
_Static_assert(sizeof(ksn_core_command_block)==3072,"core command allocation budget");
_Static_assert(sizeof(ksn_core_text_block)<=3072,"core text allocation budget");
_Static_assert(sizeof(shape_payload)<=sizeof(((ksn_command_storage *)0)->payload),"shape payload");
_Static_assert(sizeof(gradient_payload)<=sizeof(((ksn_command_storage *)0)->payload),"gradient payload");
_Static_assert(sizeof(text_payload)<=sizeof(((ksn_command_storage *)0)->payload),"text payload");
_Static_assert(sizeof(image_payload)<=sizeof(((ksn_command_storage *)0)->payload),"image payload");
_Static_assert(sizeof(stretch_payload)==12,"stretch/rotation payload");

static ksn_core_impl *impl(ksn_core *core){return &core->state;}
static const ksn_core_impl *cimpl(const ksn_core *core){return &core->state;}
static unsigned command_base(ksn_layer layer){return layer==KSN_APP?0u:KSN_APP_COMMANDS;}
static unsigned command_limit(ksn_layer layer){return layer==KSN_APP?KSN_APP_COMMANDS:KSN_SYSTEM_COMMANDS;}
static unsigned text_base(ksn_layer layer){return layer==KSN_APP?0u:KSN_APP_TEXT_BYTES;}
static unsigned text_limit(ksn_layer layer){return layer==KSN_APP?KSN_APP_TEXT_BYTES:KSN_SYSTEM_TEXT_BYTES;}
static unsigned track_base(ksn_layer layer){return layer==KSN_APP?0:KSN_APP_TRACKS;}
static unsigned track_limit(ksn_layer layer){return layer==KSN_APP?KSN_APP_TRACKS:KSN_SYSTEM_TRACKS;}
static bool running(const ksn_track *t){return t->status==KSN_ANIMATION_RUNNING||t->status==KSN_ANIMATION_PENDING;}
static bool valid_layer(ksn_layer layer){return layer==KSN_APP||layer==KSN_SYSTEM;}
static bool valid_rect(ksn_rect r){return r.x0<=r.x1&&r.y0<=r.y1;}
static unsigned rect_width(ksn_rect r){return (unsigned)((int32_t)r.x1-r.x0);}
static unsigned rect_height(ksn_rect r){return (unsigned)((int32_t)r.y1-r.y0);}
static bool valid_radius(ksn_rect r,uint8_t radius){
    return radius<=8u&&radius<=rect_width(r)/2u&&radius<=rect_height(r)/2u;
}
static ksn_ref make_ref(uint32_t generation,unsigned index){
    return (ksn_ref){(generation<<KSN_REF_INDEX_BITS)|(uint32_t)index};
}
static uint32_t ref_generation(ksn_ref ref){return ref.value>>KSN_REF_INDEX_BITS;}
static unsigned ref_index(ksn_ref ref){return ref.value&KSN_REF_INDEX_MASK;}
static ksn_result poison(ksn_core_impl *core,ksn_result result){
    if(core->poison==KSN_OK)core->poison=result;
    return result;
}
static ksn_result check_transaction(void *context,ksn_tx tx){
    ksn_endpoint *endpoint=context;ksn_core_impl *core=endpoint->core;
    if(!core->building||endpoint->layer!=core->layer||tx.value==0||tx.value!=core->transaction.value)return KSN_STALE;
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
static void payload_write(ksn_command_storage *command,const void *payload,size_t bytes){
    memset(command->payload,0,sizeof(command->payload));memcpy(command->payload,payload,bytes);
}
static void payload_read(const ksn_command_storage *command,void *payload,size_t bytes){
    memcpy(payload,command->payload,bytes);
}
static const ksn_image_entry *find_image(const ksn_core_impl *core,ksn_layer layer,ksn_resource id){
    for(unsigned i=0;i<core->image_count;i++)
        if(core->images[i].id.value==id.value&&core->images[i].layer==layer)return &core->images[i];
    return NULL;
}
static ksn_result validate_image(const ksn_core_impl *core,ksn_layer layer,ksn_resource id,uint16_t variant,uint16_t frame){
    const ksn_image_entry *entry=find_image(core,layer,id);
    if(!entry)return KSN_STALE;
    return variant<entry->port.variants&&frame<entry->port.frames?KSN_OK:KSN_INVALID;
}
static ksn_result validate_image_window(const ksn_core_impl *core,ksn_layer layer,ksn_resource id,
                                       ksn_rect bounds,uint16_t x,uint16_t y,ksn_image_scale scale,
                                       uint16_t source_width,uint16_t source_height){
    const ksn_image_entry *entry=find_image(core,layer,id);
    if(!entry)return KSN_STALE;
    if((unsigned)scale>KSN_IMAGE_STRETCH)return KSN_INVALID;
    uint32_t width=rect_width(bounds),height=rect_height(bounds);
    if(scale==KSN_IMAGE_STRETCH){
        if(x>255||y>255||!source_width||source_width>256||!source_height||source_height>256)return KSN_INVALID;
        width=source_width;height=source_height;
    }else if(scale==KSN_IMAGE_2X){
        if((width|height)&1u)return KSN_INVALID;
        width/=2;height/=2;
    }else if(scale==KSN_IMAGE_HALF){width*=2;height*=2;}
    return (uint32_t)x+width<=entry->port.width&&(uint32_t)y+height<=entry->port.height?KSN_OK:KSN_INVALID;
}

static ksn_result core_begin(void *context,ksn_update_mode mode,ksn_tx *out){
    ksn_endpoint *endpoint=context;ksn_core_impl *core=endpoint->core;
    if(!out||(mode!=KSN_REPLACE&&mode!=KSN_PATCH))return KSN_INVALID;
    if(core->building||core->submitted||core->repairing)return KSN_BUSY;
    ksn_layer layer=endpoint->layer;
    if(last_transaction==UINT32_MAX||
       (mode==KSN_REPLACE&&last_generation==KSN_REF_GENERATION_MAX))return KSN_LIMIT;
    core->building_bank=(uint8_t)(core->active^1u);
    ksn_bank *next=&core->banks[core->building_bank];
    const ksn_bank *active=&core->banks[core->active];
    ksn_command_storage *commands=next->commands;uint8_t *text=next->text;ksn_track *tracks=next->tracks;
    *next=*active;next->commands=commands;next->text=text;next->tracks=tracks;
    if(tracks)memcpy(tracks,active->tracks,sizeof(ksn_core_animation_block));
    memcpy(commands,active->commands,sizeof(ksn_core_command_block));
    memcpy(text,active->text,sizeof(ksn_core_text_block));
    core->layer=layer;core->mode=mode;core->poison=KSN_OK;core->building=true;
    core->transaction=(ksn_tx){++last_transaction};*out=core->transaction;
    if(mode==KSN_REPLACE){
        uint32_t generation=++last_generation;
        ksn_bank *bank=&core->banks[core->building_bank];
        bank->generation[layer]=generation;bank->count[layer]=0;bank->text_used[layer]=0;
        bank->background_set[layer]=false;
        memset(bank->commands+command_base(layer),0,command_limit(layer)*sizeof(ksn_command_storage));
        memset(bank->text+text_base(layer),0,text_limit(layer));
        if(bank->tracks)memset(bank->tracks+track_base(layer),0,track_limit(layer)*sizeof(ksn_track));
    }
    return KSN_OK;
}
static ksn_result core_background(void *context,ksn_tx tx,ksn_rgba color){
    ksn_core_impl *core=((ksn_endpoint *)context)->core;ksn_result result=check_transaction(context,tx);
    if(result!=KSN_OK)return result;
    if(core->layer!=KSN_APP||(color&0xffu)!=0xffu)return poison(core,KSN_INVALID);
    ksn_bank *bank=&core->banks[core->building_bank];bank->background[KSN_APP]=color;
    bank->background_set[KSN_APP]=true;return KSN_OK;
}
static ksn_result validate_draw(const ksn_draw *draw){
    if(!draw||draw->kind<KSN_RECT||draw->kind>KSN_IMAGE||!valid_rect(draw->bounds)||!valid_rect(draw->clip))return KSN_INVALID;
    switch(draw->kind){
    case KSN_RECT:return draw->data.shape.radius==0&&draw->data.shape.width==0?KSN_OK:KSN_INVALID;
    case KSN_ROUND_RECT:return draw->data.shape.width==0&&valid_radius(draw->bounds,draw->data.shape.radius)?KSN_OK:KSN_INVALID;
    case KSN_STROKE:return (draw->data.shape.width==1||draw->data.shape.width==2)&&draw->data.shape.radius==0?KSN_OK:KSN_INVALID;
    case KSN_GRADIENT:
        return draw->data.gradient.axis<=1&&valid_radius(draw->bounds,draw->data.gradient.radius)?KSN_OK:KSN_INVALID;
    case KSN_TEXT:
        if(draw->data.text.capacity<1||draw->data.text.capacity>128)return KSN_LIMIT;
        if(draw->data.text.bytes>draw->data.text.capacity)return KSN_LIMIT;
        if((unsigned)draw->data.text.font>KSN_DISPLAY)return KSN_INVALID;
        return utf8_count(draw->data.text.utf8,draw->data.text.bytes)==SIZE_MAX?KSN_INVALID:KSN_OK;
    case KSN_IMAGE:
        if(draw->data.image.rotation>1023||
           (draw->data.image.rotation&&draw->data.image.scale!=KSN_IMAGE_STRETCH))return KSN_INVALID;
        if(draw->data.image.scale==KSN_IMAGE_STRETCH&&
           (draw->data.image.variant>255||draw->data.image.frame>255))return KSN_INVALID;
        return draw->data.image.resource.value?KSN_OK:KSN_INVALID;
    }
    return KSN_INVALID;
}
static ksn_result core_add(void *context,ksn_tx tx,const ksn_draw *draw,ksn_ref *out){
    ksn_core_impl *core=((ksn_endpoint *)context)->core;ksn_result result=check_transaction(context,tx);
    if(result!=KSN_OK)return result;
    if(core->mode!=KSN_REPLACE)return poison(core,KSN_INVALID);
    if(!out)return poison(core,KSN_INVALID);
    result=validate_draw(draw);if(result!=KSN_OK)return poison(core,result);
    if(draw->kind==KSN_IMAGE){
        result=validate_image(core,core->layer,draw->data.image.resource,draw->data.image.variant,draw->data.image.frame);
        if(result!=KSN_OK)return poison(core,result);
        result=validate_image_window(core,core->layer,draw->data.image.resource,draw->bounds,
                                     draw->data.image.source_x,draw->data.image.source_y,draw->data.image.scale,
                                     draw->data.image.source_width,draw->data.image.source_height);
        if(result!=KSN_OK)return poison(core,result);
    }
    ksn_bank *bank=&core->banks[core->building_bank];ksn_layer layer=core->layer;
    if(bank->count[layer]>=command_limit(layer))return poison(core,KSN_LIMIT);
    unsigned index=command_base(layer)+bank->count[layer];
    ksn_command_storage command={.kind=(uint8_t)draw->kind,.flags=KSN_FLAG_VISIBLE,
                                .opacity=draw->opacity,.bounds=draw->bounds,.clip=draw->clip};
    switch(draw->kind){
    case KSN_RECT:case KSN_ROUND_RECT:case KSN_STROKE:{
        shape_payload payload={draw->data.shape.color,draw->data.shape.radius,draw->data.shape.width,{0,0}};
        payload_write(&command,&payload,sizeof(payload));break;
    }
    case KSN_GRADIENT:{
        gradient_payload payload={draw->data.gradient.from,draw->data.gradient.to,
                                  draw->data.gradient.axis,draw->data.gradient.radius,
                                  draw->data.gradient.dither,0};
        payload_write(&command,&payload,sizeof(payload));break;
    }
    case KSN_TEXT:{
        unsigned used=bank->text_used[layer],capacity=draw->data.text.capacity;
        if(used+capacity>text_limit(layer))return poison(core,KSN_LIMIT);
        unsigned offset=text_base(layer)+used;
        memset(bank->text+offset,0,capacity);
        if(draw->data.text.bytes)memcpy(bank->text+offset,draw->data.text.utf8,draw->data.text.bytes);
        text_payload payload={(uint16_t)offset,(uint8_t)draw->data.text.bytes,(uint8_t)capacity,
                              (uint8_t)draw->data.text.font,(uint8_t)utf8_count(draw->data.text.utf8,draw->data.text.bytes),
                              0,draw->data.text.color};
        payload_write(&command,&payload,sizeof(payload));bank->text_used[layer]=(uint16_t)(used+capacity);break;
    }
    case KSN_IMAGE:{
        command.flags|=(uint8_t)((unsigned)draw->data.image.scale<<KSN_IMAGE_SCALE_SHIFT);
        if(draw->data.image.scale==KSN_IMAGE_STRETCH){
            stretch_payload p={draw->data.image.resource.value,draw->data.image.variant,draw->data.image.frame,
                draw->data.image.rotation,draw->data.image.source_x,draw->data.image.source_y,
                draw->data.image.source_width-1,draw->data.image.source_height-1};
            payload_write(&command,&p,sizeof(p));break;
        }
        image_payload payload={draw->data.image.resource.value,draw->data.image.variant,draw->data.image.frame,
                               draw->data.image.source_x,draw->data.image.source_y};
        payload_write(&command,&payload,sizeof(payload));break;
    }
    }
    bank->commands[index]=command;bank->count[layer]++;
    *out=make_ref(bank->generation[layer],index);return KSN_OK;
}
static ksn_result referenced_command(ksn_core_impl *core,ksn_ref ref,ksn_command_storage **out){
    unsigned index=ref_index(ref),base=command_base(core->layer),limit=command_limit(core->layer);
    ksn_bank *bank=&core->banks[core->building_bank];
    if(index<base||index>=base+limit||index>=base+bank->count[core->layer]||
       ref_generation(ref)!=bank->generation[core->layer])return KSN_STALE;
    *out=&bank->commands[index];return KSN_OK;
}
static ksn_result core_change(void *context,ksn_tx tx,ksn_ref ref,const ksn_change *change){
    ksn_core_impl *core=((ksn_endpoint *)context)->core;ksn_result result=check_transaction(context,tx);
    if(result!=KSN_OK)return result;
    if(!change)return poison(core,KSN_INVALID);
    ksn_command_storage *command;result=referenced_command(core,ref,&command);
    if(result!=KSN_OK)return poison(core,result);
    if(change->property==KSN_SET_RECT||change->property==KSN_SET_ROTATION){
        ksn_track *tracks=core->banks[core->building_bank].tracks;
        if(tracks)for(unsigned i=track_base(core->layer);i<track_base(core->layer)+track_limit(core->layer);i++)
            if(running(&tracks[i])&&tracks[i].target.value==ref.value)return poison(core,KSN_BUSY);
    }
    switch(change->property){
    case KSN_SET_RECT:
        if(!valid_rect(change->value.rect))return poison(core,KSN_INVALID);
        if(command->kind==KSN_IMAGE){
            image_payload p;payload_read(command,&p,sizeof(p));
            ksn_image_scale scale=(ksn_image_scale)(command->flags>>KSN_IMAGE_SCALE_SHIFT);
            uint16_t width=0,height=0;
            if(scale==KSN_IMAGE_STRETCH){stretch_payload s;payload_read(command,&s,sizeof(s));
                width=(uint16_t)s.w+1;height=(uint16_t)s.h+1;p.source_x=s.x;p.source_y=s.y;}
            result=validate_image_window(core,core->layer,(ksn_resource){p.resource},change->value.rect,
                                         p.source_x,p.source_y,scale,width,height);
            if(result!=KSN_OK)return poison(core,result);
        }
        if(command->kind==KSN_ROUND_RECT){shape_payload p;payload_read(command,&p,sizeof(p));
            if(!valid_radius(change->value.rect,p.radius))return poison(core,KSN_INVALID);}
        if(command->kind==KSN_GRADIENT){gradient_payload p;payload_read(command,&p,sizeof(p));
            if(!valid_radius(change->value.rect,p.radius))return poison(core,KSN_INVALID);}
        command->bounds=change->value.rect;return KSN_OK;
    case KSN_SET_CLIP:
        if(!valid_rect(change->value.rect))return poison(core,KSN_INVALID);
        command->clip=change->value.rect;return KSN_OK;
    case KSN_SET_COLOR:
        if(command->kind==KSN_RECT||command->kind==KSN_ROUND_RECT||command->kind==KSN_STROKE){
            shape_payload p;payload_read(command,&p,sizeof(p));p.color=change->value.color;payload_write(command,&p,sizeof(p));return KSN_OK;
        }
        if(command->kind==KSN_TEXT){text_payload p;payload_read(command,&p,sizeof(p));p.color=change->value.color;payload_write(command,&p,sizeof(p));return KSN_OK;}
        return poison(core,KSN_UNSUPPORTED);
    case KSN_SET_TEXT:{
        if(command->kind!=KSN_TEXT)return poison(core,KSN_INVALID);
        text_payload p;payload_read(command,&p,sizeof(p));
        if(change->value.text.bytes>p.capacity)return poison(core,KSN_LIMIT);
        size_t count=utf8_count(change->value.text.utf8,change->value.text.bytes);
        if(count==SIZE_MAX)return poison(core,KSN_INVALID);
        ksn_bank *bank=&core->banks[core->building_bank];
        memset(bank->text+p.offset,0,p.capacity);
        if(change->value.text.bytes)memcpy(bank->text+p.offset,change->value.text.utf8,change->value.text.bytes);
        p.length=(uint8_t)change->value.text.bytes;p.reveal=(uint8_t)count;payload_write(command,&p,sizeof(p));return KSN_OK;
    }
    case KSN_SET_REVEAL:{
        if(command->kind!=KSN_TEXT)return poison(core,KSN_INVALID);
        text_payload p;payload_read(command,&p,sizeof(p));ksn_bank *bank=&core->banks[core->building_bank];
        size_t count=utf8_count((const char *)bank->text+p.offset,p.length);
        if(change->value.reveal>count)return poison(core,KSN_INVALID);
        p.reveal=(uint8_t)change->value.reveal;payload_write(command,&p,sizeof(p));return KSN_OK;
    }
    case KSN_SET_VISIBLE:
        if(change->value.visible)command->flags|=KSN_FLAG_VISIBLE;else command->flags&=(uint8_t)~KSN_FLAG_VISIBLE;
        return KSN_OK;
    case KSN_SET_ROTATION:{
        if(command->kind!=KSN_IMAGE||command->flags>>KSN_IMAGE_SCALE_SHIFT!=KSN_IMAGE_STRETCH||
           change->value.rotation>1023)return poison(core,KSN_INVALID);
        stretch_payload p;payload_read(command,&p,sizeof(p));p.rotation=change->value.rotation;
        payload_write(command,&p,sizeof(p));return KSN_OK;
    }
    case KSN_SET_IMAGE_FRAME:{
        if(command->kind!=KSN_IMAGE)return poison(core,KSN_INVALID);
        image_payload p;payload_read(command,&p,sizeof(p));
        result=validate_image(core,core->layer,(ksn_resource){p.resource},change->value.image.variant,change->value.image.frame);
        if(result!=KSN_OK)return poison(core,result);
        if(command->flags>>KSN_IMAGE_SCALE_SHIFT==KSN_IMAGE_STRETCH){
            if(change->value.image.variant>255||change->value.image.frame>255)return poison(core,KSN_INVALID);
            stretch_payload s;payload_read(command,&s,sizeof(s));
            s.variant=(uint8_t)change->value.image.variant;s.frame=(uint8_t)change->value.image.frame;
            payload_write(command,&s,sizeof(s));return KSN_OK;
        }
        p.variant=change->value.image.variant;p.frame=change->value.image.frame;
        payload_write(command,&p,sizeof(p));return KSN_OK;
    }
    }
    return poison(core,KSN_INVALID);
}
static void apply_pose(ksn_command_storage *command,ksn_pose pose){
    stretch_payload p;payload_read(command,&p,sizeof(p));
    command->bounds=pose.bounds;p.rotation=(uint16_t)((pose.rotation%1024+1024)%1024);
    payload_write(command,&p,sizeof(p));
}
static ksn_track *find_track(ksn_bank *bank,ksn_layer layer,ksn_animation id){
    if(!bank->tracks||!id.value)return NULL;
    for(unsigned i=track_base(layer);i<track_base(layer)+track_limit(layer);i++)
        if(bank->tracks[i].id.value==id.value)return &bank->tracks[i];
    return NULL;
}
static ksn_result core_animate(void *context,ksn_tx tx,const ksn_motion *motion,ksn_animation *out){
    ksn_core_impl *core=((ksn_endpoint *)context)->core;ksn_result result=check_transaction(context,tx);
    if(result!=KSN_OK)return result;
    if(!motion||!out||motion->count!=1||motion->property!=KSN_TRANSFORM||
       !valid_rect(motion->from.pose.bounds)||!valid_rect(motion->to.pose.bounds)||
       !motion->duration_ms||motion->duration_ms>86400000u||
       (unsigned)motion->easing>KSN_STEP||(unsigned)motion->repeat>KSN_PINGPONG)return poison(core,KSN_INVALID);
    ksn_command_storage *command;result=referenced_command(core,motion->first,&command);
    if(result!=KSN_OK)return poison(core,result);
    if(command->kind!=KSN_IMAGE||command->flags>>KSN_IMAGE_SCALE_SHIFT!=KSN_IMAGE_STRETCH)
        return poison(core,KSN_UNSUPPORTED);
    ksn_bank *bank=&core->banks[core->building_bank];
    if(!bank->tracks)return poison(core,KSN_UNSUPPORTED);
    ksn_track *slot=NULL;
    for(unsigned i=track_base(core->layer);i<track_base(core->layer)+track_limit(core->layer);i++){
        ksn_track *t=&bank->tracks[i];
        if(running(t)&&t->target.value==motion->first.value)return poison(core,KSN_BUSY);
        if(!running(t)&&!slot)slot=t;
    }
    if(!slot||last_animation==UINT32_MAX)return poison(core,KSN_LIMIT);
    *slot=(ksn_track){.from=motion->from.pose,.to=motion->to.pose,.id={++last_animation},.target=motion->first,
        .duration_ms=motion->duration_ms,.easing=(uint8_t)motion->easing,.repeat=(uint8_t)motion->repeat,
        .status=KSN_ANIMATION_PENDING};
    apply_pose(command,slot->from);*out=slot->id;return KSN_OK;
}
static ksn_result core_stop(void *context,ksn_tx tx,ksn_animation animation){
    ksn_core_impl *core=((ksn_endpoint *)context)->core;ksn_result result=check_transaction(context,tx);
    if(result!=KSN_OK)return result;
    ksn_track *t=find_track(&core->banks[core->building_bank],core->layer,animation);
    if(!t)return poison(core,KSN_STALE);
    if(running(t))t->status=KSN_ANIMATION_STOPPED;
    return KSN_OK;
}
static void sample_tracks(ksn_core_impl *core,uint64_t now,bool reduce);
static ksn_result core_end(void *context,ksn_tx tx){
    ksn_core_impl *core=((ksn_endpoint *)context)->core;ksn_result result=check_transaction(context,tx);
    if(result!=KSN_OK)return result;
    if(!core->banks[core->building_bank].background_set[KSN_APP])return poison(core,KSN_INVALID);
    /* Coalesce native motion into a guest update before sealing the bank. */
    sample_tracks(core,core->animation_now_us,false);
    core->building=false;core->submitted=true;
    core->outcome=(ksn_submission){tx,KSN_SUBMITTED,KSN_OK,core->layer};return KSN_OK;
}
static void core_abort(void *context,ksn_tx tx){
    ksn_core_impl *core=((ksn_endpoint *)context)->core;
    if(core->building&&((ksn_endpoint *)context)->layer==core->layer&&
       tx.value==core->transaction.value)core->building=false;
}
static ksn_limits core_limits(void *context){
    ksn_core_impl *core=((ksn_endpoint *)context)->core;
    return (ksn_limits){{KSN_APP_COMMANDS,KSN_APP_TEXT_BYTES,KSN_APP_TRACKS},
                       {KSN_SYSTEM_COMMANDS,KSN_SYSTEM_TEXT_BYTES,KSN_SYSTEM_TRACKS},
                       KSN_CORE_RESERVED_BYTES+4*sizeof(uint32_t)+(core->banks[0].tracks?2*sizeof(ksn_core_animation_block):0),0};
}
static uint8_t track_usage(const ksn_bank *bank,ksn_layer layer){
    uint8_t count=0;
    if(bank->tracks)for(unsigned i=track_base(layer);i<track_base(layer)+track_limit(layer);i++)count+=running(&bank->tracks[i]);
    return count;
}
static ksn_stats core_stats(void *context){
    ksn_core_impl *core=((ksn_endpoint *)context)->core;const ksn_bank *bank=&core->banks[core->active];
    ksn_stats stats={0};
    for(unsigned layer=0;layer<2;layer++)stats.used[layer]=(ksn_capacity){bank->count[layer],bank->text_used[layer],track_usage(bank,(ksn_layer)layer)};
    stats.native_current=KSN_CORE_RESERVED_BYTES+4*sizeof(uint32_t)+(bank->tracks?2*sizeof(ksn_core_animation_block):0);
    stats.native_peak=stats.native_current;return stats;
}
static const ksn_api core_api={core_begin,core_background,core_add,core_change,core_animate,
                              core_stop,core_end,core_abort,core_limits,core_stats};

void ksn_core_init(ksn_core *storage){
    if(!storage)return;
    ksn_core_impl *core=impl(storage);
    ksn_command_storage *commands[2]={core->banks[0].commands,core->banks[1].commands};
    uint8_t *text[2]={core->banks[0].text,core->banks[1].text};
    ksn_track *tracks[2]={core->banks[0].tracks,core->banks[1].tracks};
    if(!commands[0]||!commands[1]||!text[0]||!text[1])return;
    memset(storage,0,sizeof(*storage));
    for(unsigned i=0;i<2;i++){
        core->banks[i].commands=commands[i];core->banks[i].text=text[i];
        core->banks[i].tracks=tracks[i];
        if(tracks[i])memset(tracks[i],0,sizeof(ksn_core_animation_block));
        memset(commands[i],0,sizeof(ksn_core_command_block));
        memset(text[i],0,sizeof(ksn_core_text_block));
    }
    for(unsigned layer=0;layer<2;layer++){
        core->endpoints[layer]=(ksn_endpoint){core,(ksn_layer)layer};
    }
    /* SYSTEM can paint before the first APP submission. APP REPLACE still
     * requires an explicit opaque background. */
    core->banks[0].background[KSN_APP]=0x000000ff;
    core->banks[0].background_set[KSN_APP]=true;
    core->full_redraw=true;
}
ksn_result ksn_core_bind(ksn_core *storage,ksn_core_command_block *c0,ksn_core_command_block *c1,
                         ksn_core_text_block *t0,ksn_core_text_block *t1){
    if(!storage||!c0||!c1||!t0||!t1||c0==c1||t0==t1)return KSN_INVALID;
    *storage=(ksn_core){0};
    storage->state.banks[0].commands=c0->commands;storage->state.banks[1].commands=c1->commands;
    storage->state.banks[0].text=t0->bytes;storage->state.banks[1].text=t1->bytes;
    ksn_core_init(storage);return KSN_OK;
}
ksn_client ksn_core_client(ksn_core *storage,ksn_layer layer){
    if(!storage||!valid_layer(layer))return (ksn_client){0};
    ksn_core_impl *core=impl(storage);return (ksn_client){&core_api,&core->endpoints[layer]};
}
ksn_result ksn_core_reset_layer(ksn_core *storage,ksn_layer layer){
    if(!storage||!valid_layer(layer))return KSN_INVALID;
    ksn_core_impl *core=impl(storage);
    if(core->repairing||((core->building||core->submitted)&&core->layer==layer))return KSN_BUSY;
    for(unsigned i=0;i<2;i++){
        ksn_bank *bank=&core->banks[i];
        memset(bank->commands+command_base(layer),0,command_limit(layer)*sizeof(ksn_command_storage));
        memset(bank->text+text_base(layer),0,text_limit(layer));
        bank->count[layer]=bank->text_used[layer]=0;bank->generation[layer]=0;
        if(bank->tracks)memset(bank->tracks+track_base(layer),0,track_limit(layer)*sizeof(ksn_track));
        bank->background[layer]=layer==KSN_APP?0x000000ff:0;
        bank->background_set[layer]=layer==KSN_APP;
    }
    unsigned kept=0;
    for(unsigned i=0;i<core->image_count;i++)
        if(core->images[i].layer!=layer)core->images[kept++]=core->images[i];
    memset(core->images+kept,0,(core->image_count-kept)*sizeof(*core->images));
    core->image_count=(uint8_t)kept;core->invalidated=true;
    return KSN_OK;
}
ksn_result ksn_core_enable_animation(ksn_core *storage,ksn_core_animation_block *a,ksn_core_animation_block *b){
    if(!storage||!a||!b||a==b)return KSN_INVALID;
    ksn_core_impl *core=impl(storage);
    if(core->submitted||core->repairing||core->banks[0].tracks||core->banks[1].tracks)return KSN_BUSY;
    memset(a,0,sizeof(*a));memset(b,0,sizeof(*b));
    core->banks[0].tracks=a->tracks;core->banks[1].tracks=b->tracks;return KSN_OK;
}
uint32_t ksn_core_animation_bytes(const ksn_core *storage){
    return storage&&cimpl(storage)->banks[0].tracks?2*sizeof(ksn_core_animation_block):0;
}
ksn_result ksn_core_finish_animation(ksn_core *storage,ksn_layer layer,ksn_tx tx,ksn_animation id){
    if(!storage||!valid_layer(layer))return KSN_INVALID;
    ksn_core_impl *core=impl(storage);ksn_result r=check_transaction(&core->endpoints[layer],tx);
    if(r!=KSN_OK)return r;
    ksn_track *t=find_track(&core->banks[core->building_bank],layer,id);
    if(!t)return poison(core,KSN_STALE);
    ksn_command_storage *command;r=referenced_command(core,t->target,&command);
    if(r!=KSN_OK)return poison(core,r);
    apply_pose(command,t->to);t->status=KSN_ANIMATION_FINISHED;return KSN_OK;
}
ksn_animation_status ksn_core_poll_animation(const ksn_core *storage,ksn_layer layer,ksn_animation id){
    if(!storage||!valid_layer(layer)||!id.value)return KSN_ANIMATION_DISCARDED;
    const ksn_core_impl *core=cimpl(storage);const ksn_bank *active=&core->banks[core->active];
    if(active->tracks)for(unsigned i=track_base(layer);i<track_base(layer)+track_limit(layer);i++)
        if(active->tracks[i].id.value==id.value)return (ksn_animation_status)active->tracks[i].status;
    const ksn_bank *next=&core->banks[core->building_bank];
    if((core->building||core->submitted)&&next->tracks)
        for(unsigned i=track_base(layer);i<track_base(layer)+track_limit(layer);i++)
            if(next->tracks[i].id.value==id.value)return KSN_ANIMATION_PENDING;
    return KSN_ANIMATION_DISCARDED;
}
void ksn_core_start_animations(ksn_core *storage,uint64_t now){
    if(!storage)return;
    ksn_core_impl *core=impl(storage);
    core->animation_now_us=now;
    if(core->building||core->submitted||core->repairing)return;
    ksn_track *tracks=core->banks[core->active].tracks;if(!tracks)return;
    for(unsigned i=0;i<KSN_TRACKS;i++)if(tracks[i].status==KSN_ANIMATION_PENDING){
        tracks[i].started_us=tracks[i].sampled_us=now;tracks[i].status=KSN_ANIMATION_RUNNING;
    }
}
uint64_t ksn_core_animation_deadline(const ksn_core *storage){
    if(!storage)return UINT64_MAX;
    const ksn_track *tracks=cimpl(storage)->banks[cimpl(storage)->active].tracks;
    uint64_t next=UINT64_MAX;if(!tracks)return next;
    for(unsigned i=0;i<KSN_TRACKS;i++)if(tracks[i].status==KSN_ANIMATION_RUNNING){
        uint64_t due=tracks[i].sampled_us>UINT64_MAX-33334?UINT64_MAX:tracks[i].sampled_us+33334;
        if(tracks[i].easing==KSN_STEP&&tracks[i].repeat==KSN_ONCE){
            uint64_t duration=(uint64_t)tracks[i].duration_ms*1000;
            uint64_t end=tracks[i].started_us>UINT64_MAX-duration?UINT64_MAX:tracks[i].started_us+duration;
            if(end>due)due=end;
        }
        if(due<next)next=due;
    }
    return next;
}
static int32_t motion_lerp(int32_t a,int32_t b,uint32_t q){
    int64_t value=(int64_t)a*65536+((int64_t)b-a)*q;
    return (int32_t)(value<0?-((-value+32768)/65536):(value+32768)/65536);
}
static uint32_t motion_progress(const ksn_track *t,uint64_t elapsed,bool reduce,bool *done){
    uint64_t duration=(uint64_t)t->duration_ms*1000;
    *done=reduce||(t->repeat==KSN_ONCE&&elapsed>=duration);
    if(*done)return 65536;
    if(t->repeat==KSN_LOOP)elapsed%=duration;
    else if(t->repeat==KSN_PINGPONG){elapsed%=duration*2;if(elapsed>duration)elapsed=duration*2-elapsed;}
    uint64_t q=elapsed*65536/duration,r=65536-q;
    if(t->easing==KSN_EASE_OUT_CUBIC)q=65536-(r*r*r>>32);
    else if(t->easing==KSN_EASE_IN_OUT_CUBIC)q=q<32768?(4*q*q*q>>32):65536-(4*r*r*r>>32);
    else if(t->easing==KSN_STEP)q=q==65536?65536:0;
    return (uint32_t)q;
}
static void sample_tracks(ksn_core_impl *core,uint64_t now,bool reduce){
    ksn_bank *bank=&core->banks[core->building_bank];
    if(!bank->tracks)return;
    for(unsigned i=0;i<KSN_TRACKS;i++){
        ksn_track *t=&bank->tracks[i];if(t->status!=KSN_ANIMATION_RUNNING)continue;
        if(!reduce&&(now<t->sampled_us||now-t->sampled_us<33334))continue;
        uint64_t elapsed=now>=t->started_us?now-t->started_us:0;bool done;
        uint32_t q=motion_progress(t,elapsed,reduce,&done);
        ksn_pose p={.bounds={motion_lerp(t->from.bounds.x0,t->to.bounds.x0,q),motion_lerp(t->from.bounds.y0,t->to.bounds.y0,q),
            motion_lerp(t->from.bounds.x1,t->to.bounds.x1,q),motion_lerp(t->from.bounds.y1,t->to.bounds.y1,q)},
            .rotation=motion_lerp(t->from.rotation,t->to.rotation,q)};
        apply_pose(&bank->commands[ref_index(t->target)],p);t->sampled_us=now;
        if(done)t->status=KSN_ANIMATION_FINISHED;
    }
}
void ksn_core_set_animation_time(ksn_core *storage,uint64_t now){if(storage)impl(storage)->animation_now_us=now;}
ksn_result ksn_core_advance_animations(ksn_core *storage,uint64_t now,bool reduce,ksn_tx *out){
    if(!storage||!out)return KSN_INVALID;
    *out=(ksn_tx){0};ksn_core_impl *core=impl(storage);core->animation_now_us=now;
    if(core->building||core->submitted||core->repairing)return KSN_BUSY;
    uint64_t deadline=ksn_core_animation_deadline(storage);
    if(deadline==UINT64_MAX||(!reduce&&now<deadline))return KSN_OK;
    ksn_tx tx;ksn_result r=core_begin(&core->endpoints[KSN_APP],KSN_PATCH,&tx);if(r!=KSN_OK)return r;
    sample_tracks(core,now,reduce);
    r=core_end(&core->endpoints[KSN_APP],tx);
    if(r==KSN_OK)*out=tx;else core_abort(&core->endpoints[KSN_APP],tx);
    return r;
}

ksn_result ksn_core_register_image(ksn_core *storage,ksn_layer layer,const ksn_image_port *port,ksn_resource *out){
    if(!storage||!valid_layer(layer)||!port||!out||!port->read_span||
       !port->width||!port->height||!port->variants||!port->frames)return KSN_INVALID;
    ksn_core_impl *core=impl(storage);
    if(core->building||core->submitted||core->repairing)return KSN_BUSY;
    if(core->image_count==KSN_RESOURCES||last_resource==UINT32_MAX)return KSN_LIMIT;
    ksn_image_entry *entry=&core->images[core->image_count++];
    *entry=(ksn_image_entry){*port,{++last_resource},layer};*out=entry->id;return KSN_OK;
}
bool ksn_core_has_submission(const ksn_core *storage){return storage&&cimpl(storage)->submitted;}
ksn_submission ksn_core_poll(const ksn_core *storage){
    return storage?cimpl(storage)->outcome:(ksn_submission){0};
}
bool ksn_core_needs_repair(const ksn_core *storage){
    return storage&&(cimpl(storage)->full_redraw||cimpl(storage)->invalidated);
}
void ksn_core_invalidate(ksn_core *storage){if(storage)impl(storage)->invalidated=true;}
ksn_result ksn_core_check_builder(const ksn_core *storage,ksn_tx ticket,ksn_layer layer,ksn_update_mode mode){
    if(!storage)return KSN_INVALID;
    const ksn_core_impl *core=cimpl(storage);
    if(!core->building||!ticket.value||core->transaction.value!=ticket.value)return KSN_STALE;
    if(core->layer!=layer||core->mode!=mode)return KSN_INVALID;
    return core->poison;
}
ksn_result ksn_core_builder_usage(const ksn_core *storage,ksn_tx ticket,ksn_capacity *out){
    if(!storage||!out)return KSN_INVALID;
    const ksn_core_impl *core=cimpl(storage);
    if(!core->building||!ticket.value||core->transaction.value!=ticket.value)return KSN_STALE;
    const ksn_bank *bank=&core->banks[core->building_bank];
    *out=(ksn_capacity){bank->count[core->layer],bank->text_used[core->layer],0};return KSN_OK;
}

ksn_result ksn_core_group(ksn_core *storage,ksn_layer layer,ksn_tx tx,ksn_ref first,uint16_t count,uint8_t opacity){
    if(!storage||!valid_layer(layer)||!count)return KSN_INVALID;
    ksn_core_impl *core=impl(storage);
    ksn_result result=check_transaction(&core->endpoints[layer],tx);
    if(result!=KSN_OK)return result;
    ksn_command_storage *command;
    result=referenced_command(core,first,&command);
    if(result!=KSN_OK)return poison(core,result);
    unsigned index=ref_index(first)-command_base(layer);
    if(count>core->banks[core->building_bank].count[layer]-index)return poison(core,KSN_INVALID);
    bool existing=(command->flags&KSN_FLAG_GROUP)!=0;
    if(!existing&&core->mode!=KSN_REPLACE)return poison(core,KSN_INVALID);
    for(unsigned i=0;i<count;i++){
        unsigned expected=KSN_FLAG_GROUP|(i==0?KSN_FLAG_GROUP_BEGIN:0)|(i+1==count?KSN_FLAG_GROUP_END:0);
        unsigned flags=command[i].flags&(KSN_FLAG_GROUP|KSN_FLAG_GROUP_BEGIN|KSN_FLAG_GROUP_END);
        if(flags!=(existing?expected:0))return poison(core,KSN_INVALID);
    }
    for(unsigned i=0;i<count;i++){
        command[i].flags|=KSN_FLAG_GROUP|(i==0?KSN_FLAG_GROUP_BEGIN:0)|(i+1==count?KSN_FLAG_GROUP_END:0);
        command[i].reserved=opacity;
    }
    return KSN_OK;
}
ksn_result ksn_core_presented(ksn_core *storage,ksn_tx ticket){
    if(!storage)return KSN_INVALID;
    ksn_core_impl *core=impl(storage);
    if((!core->submitted&&!core->repairing)||ticket.value!=core->transaction.value)return KSN_STALE;
    if(core->repairing){
        core->repairing=false;core->full_redraw=false;return KSN_OK;
    }
    core->active=core->building_bank;core->submitted=false;core->full_redraw=false;
    core->outcome=(ksn_submission){ticket,KSN_PRESENTED,KSN_OK,core->layer};return KSN_OK;
}
ksn_result ksn_core_discard(ksn_core *storage,ksn_tx ticket){
    return ksn_core_discard_reason(storage,ticket,KSN_OK);
}
ksn_result ksn_core_discard_reason(ksn_core *storage,ksn_tx ticket,ksn_result reason){
    if(!storage)return KSN_INVALID;
    ksn_core_impl *core=impl(storage);
    if(!core->submitted||ticket.value!=core->transaction.value)return KSN_STALE;
    core->submitted=false;
    core->outcome=(ksn_submission){ticket,KSN_DISCARDED,reason,core->layer};return KSN_OK;
}
static ksn_capacity usage(const ksn_bank *bank,ksn_layer layer){
    if(!valid_layer(layer))return (ksn_capacity){0};
    return (ksn_capacity){bank->count[layer],bank->text_used[layer],track_usage(bank,layer)};
}
ksn_capacity ksn_core_active_usage(const ksn_core *storage,ksn_layer layer){
    if(!storage)return (ksn_capacity){0};
    const ksn_core_impl *core=cimpl(storage);return usage(&core->banks[core->active],layer);
}
ksn_capacity ksn_core_submission_usage(const ksn_core *storage,ksn_layer layer){
    if(!storage)return (ksn_capacity){0};
    const ksn_core_impl *core=cimpl(storage);
    return core->submitted?usage(&core->banks[core->building_bank],layer):(ksn_capacity){0};
}
bool ksn_core_refs_active(const ksn_core *storage,ksn_layer layer,ksn_ref first,uint16_t count){
    if(!storage||!valid_layer(layer)||!count)return false;
    const ksn_core_impl *core=cimpl(storage);const ksn_bank *bank=&core->banks[core->active];
    unsigned index=ref_index(first),base=command_base(layer),limit=command_limit(layer);
    return index>=base&&index+count<=base+limit&&index+count<=base+bank->count[layer]&&
           ref_generation(first)==bank->generation[layer];
}

ksn_result ksn_core_frame(const ksn_core *storage,ksn_frame *out){
    if(!storage||!out)return KSN_INVALID;
    const ksn_core_impl *core=cimpl(storage);
    if(!core->submitted&&!core->repairing)return KSN_STALE;
    const ksn_bank *before=&core->banks[core->active];
    const ksn_bank *after=&core->banks[core->repairing?core->active:core->building_bank];
    *out=(ksn_frame){.ticket=core->transaction,.previous_background=before->background[KSN_APP],
                    .next_background=after->background[KSN_APP],
                    .full_redraw=core->full_redraw||core->invalidated};
    for(unsigned i=0;i<2;i++){
        out->previous[i]=usage(before,(ksn_layer)i);out->next[i]=usage(after,(ksn_layer)i);
    }
    return KSN_OK;
}
ksn_result ksn_core_prepare_frame(ksn_core *storage,ksn_frame *out){
    if(!storage||!out)return KSN_INVALID;
    ksn_core_impl *core=impl(storage);
    if(core->building)return KSN_BUSY;
    if(!core->submitted&&!core->repairing){
        if(!ksn_core_needs_repair(storage))return KSN_STALE;
        if(last_transaction==UINT32_MAX)return KSN_LIMIT;
        core->transaction=(ksn_tx){++last_transaction};core->repairing=true;
    }
    /* A later invalidate, including one inside the display callback, survives
     * the current acknowledgement. IO failure keeps full_redraw set too. */
    core->full_redraw|=core->invalidated;core->invalidated=false;
    return ksn_core_frame(storage,out);
}
void ksn_core_defer_repair(ksn_core *storage,ksn_tx ticket){
    if(!storage)return;
    ksn_core_impl *core=impl(storage);
    if(core->repairing&&ticket.value==core->transaction.value)core->repairing=false;
}
ksn_result ksn_core_failed(ksn_core *storage,ksn_tx ticket){
    if(!storage)return KSN_INVALID;
    ksn_core_impl *core=impl(storage);
    if((!core->submitted&&!core->repairing)||ticket.value!=core->transaction.value)return KSN_STALE;
    core->full_redraw=true;
    if(core->submitted)core->outcome.reason=KSN_IO;
    return KSN_OK;
}
ksn_result ksn_core_read(const ksn_core *storage,ksn_tx ticket,bool previous,
                       ksn_layer layer,uint16_t index,ksn_frame_command *out){
    if(!storage||!out||!valid_layer(layer))return KSN_INVALID;
    const ksn_core_impl *core=cimpl(storage);
    if((!core->submitted&&!core->repairing)||ticket.value!=core->transaction.value)return KSN_STALE;
    const ksn_bank *bank=&core->banks[(previous||core->repairing)?core->active:core->building_bank];
    if(index>=bank->count[layer])return KSN_INVALID;
    const ksn_command_storage *command=&bank->commands[command_base(layer)+index];
    memset(out,0,sizeof(*out));
    ksn_draw *draw=&out->draw;
    draw->kind=(ksn_kind)command->kind;draw->bounds=command->bounds;
    draw->clip=command->clip;draw->opacity=command->opacity;
    out->visible=(command->flags&KSN_FLAG_VISIBLE)!=0;
    out->group_begin=(command->flags&KSN_FLAG_GROUP_BEGIN)!=0;
    out->group_end=(command->flags&KSN_FLAG_GROUP_END)!=0;
    out->group_opacity=command->reserved;
    switch(draw->kind){
    case KSN_RECT:case KSN_ROUND_RECT:case KSN_STROKE:{
        shape_payload p;payload_read(command,&p,sizeof(p));
        draw->data.shape.color=p.color;draw->data.shape.radius=p.radius;draw->data.shape.width=p.width;break;
    }
    case KSN_GRADIENT:{
        gradient_payload p;payload_read(command,&p,sizeof(p));
        draw->data.gradient.from=p.from;draw->data.gradient.to=p.to;
        draw->data.gradient.axis=p.axis;draw->data.gradient.radius=p.radius;
        draw->data.gradient.dither=p.dither!=0;break;
    }
    case KSN_TEXT:{
        text_payload p;payload_read(command,&p,sizeof(p));
        memcpy(out->text,bank->text+p.offset,p.length);
        draw->data.text.utf8=out->text;draw->data.text.bytes=p.length;
        draw->data.text.capacity=p.capacity;draw->data.text.font=(ksn_font)p.font;
        draw->data.text.color=p.color;out->reveal=p.reveal;break;
    }
    case KSN_IMAGE:{
        image_payload p;payload_read(command,&p,sizeof(p));
        draw->data.image.resource=(ksn_resource){p.resource};
        draw->data.image.variant=p.variant;draw->data.image.frame=p.frame;
        draw->data.image.source_x=p.source_x;draw->data.image.source_y=p.source_y;
        draw->data.image.scale=(ksn_image_scale)(command->flags>>KSN_IMAGE_SCALE_SHIFT);
        if(draw->data.image.scale==KSN_IMAGE_STRETCH){
            stretch_payload s;payload_read(command,&s,sizeof(s));
            draw->data.image.variant=s.variant;draw->data.image.frame=s.frame;draw->data.image.rotation=s.rotation;
            draw->data.image.source_width=(uint16_t)s.w+1;draw->data.image.source_height=(uint16_t)s.h+1;
            draw->data.image.source_x=s.x;draw->data.image.source_y=s.y;
        }
        break;
    }
    default:return KSN_INVALID;
    }
    return KSN_OK;
}

ksn_result ksn_core_image_span(const ksn_core *storage,ksn_tx ticket,bool previous,
                            ksn_layer layer,uint16_t index,uint16_t y,uint16_t x,
                            uint16_t count,uint16_t *rgb565,uint8_t *alpha){
    if(!storage||!valid_layer(layer))return KSN_INVALID;
    const ksn_core_impl *core=cimpl(storage);
    if((!core->submitted&&!core->repairing)||ticket.value!=core->transaction.value)return KSN_STALE;
    const ksn_bank *bank=&core->banks[(previous||core->repairing)?core->active:core->building_bank];
    if(index>=bank->count[layer])return KSN_INVALID;
    const ksn_command_storage *command=&bank->commands[command_base(layer)+index];
    if(command->kind!=KSN_IMAGE)return KSN_INVALID;
    image_payload p;payload_read(command,&p,sizeof(p));
    if(command->flags>>KSN_IMAGE_SCALE_SHIFT==KSN_IMAGE_STRETCH){stretch_payload s;
        payload_read(command,&s,sizeof(s));p.variant=s.variant;p.frame=s.frame;}
    const ksn_image_entry *entry=find_image(core,layer,(ksn_resource){p.resource});
    if(!entry)return KSN_STALE;
    if(y>=entry->port.height||x>entry->port.width||count>entry->port.width-x||
       (count&&(!rgb565||!alpha)))return KSN_INVALID;
    if(!count)return KSN_OK;
    return entry->port.read_span(entry->port.ctx,p.variant,p.frame,y,x,count,rgb565,alpha);
}

static uint32_t command_bands(const ksn_command_storage *command){
    if(!(command->flags&KSN_FLAG_VISIBLE)||!command->opacity)return 0;
    ksn_rect bounds=command->bounds;
    if(command->kind==KSN_IMAGE&&command->flags>>KSN_IMAGE_SCALE_SHIFT==KSN_IMAGE_STRETCH){
        stretch_payload p;payload_read(command,&p,sizeof(p));bounds=ksn_image_footprint(bounds,p.rotation);
    }
    int32_t x0=bounds.x0,x1=bounds.x1,y0=bounds.y0,y1=bounds.y1;
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
ksn_result ksn_core_damage(const ksn_core *storage,ksn_tx ticket,uint32_t *bands){
    if(!storage||!bands)return KSN_INVALID;
    const ksn_core_impl *core=cimpl(storage);
    if((!core->submitted&&!core->repairing)||ticket.value!=core->transaction.value)return KSN_STALE;
    const ksn_bank *old=&core->banks[core->active],*next=&core->banks[core->building_bank];
    *bands=0;
    if(core->repairing||core->full_redraw||core->invalidated||old->background[KSN_APP]!=next->background[KSN_APP]||
       old->generation[0]!=next->generation[0]||old->generation[1]!=next->generation[1]){
        *bands=(1u<<17)-1u;return KSN_OK;
    }
    for(unsigned layer=0;layer<2;layer++)for(unsigned i=0;i<next->count[layer];i++){
        unsigned index=command_base((ksn_layer)layer)+i;
        const ksn_command_storage *a=&old->commands[index],*b=&next->commands[index];
        bool changed=memcmp(a,b,sizeof(*a))!=0;
        if(!changed&&b->kind==KSN_TEXT){
            text_payload p;payload_read(b,&p,sizeof(p));
            changed=memcmp(old->text+p.offset,next->text+p.offset,p.length)!=0;
        }
        if(changed)*bands|=command_bands(a)|command_bands(b);
    }
    return KSN_OK;
}
