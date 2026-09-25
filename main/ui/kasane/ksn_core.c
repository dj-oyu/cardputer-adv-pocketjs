#include "ksn_core.h"
#include "ksn_image_transform.h"
#include "ksn_p0_probe.h"
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
static unsigned command_owner(const ksn_bank *bank,unsigned index){
    return (bank->command_owner[index/32u]>>(index%32u))&1u;
}
static void select_command_owner(ksn_bank *bank,unsigned index,unsigned owner){
    uint32_t bit=1u<<(index%32u);
    if(owner)bank->command_owner[index/32u]|=bit;
    else bank->command_owner[index/32u]&=~bit;
}
static ksn_command_storage *bank_command(ksn_bank *bank,unsigned index){
    return (command_owner(bank,index)==bank->physical_index?
            bank->commands:bank->command_peer)+index;
}
static const ksn_command_storage *bank_command_const(const ksn_bank *bank,unsigned index){
    return (command_owner(bank,index)==bank->physical_index?
            bank->commands:bank->command_peer)+index;
}
static ksn_command_storage *private_command(ksn_core_impl *core,unsigned index){
    ksn_bank *next=&core->banks[core->building_bank];
    const ksn_bank *active=&core->banks[core->active];
    if(command_owner(next,index)==command_owner(active,index)){
        unsigned owner=command_owner(active,index)^1u;
        ksn_command_storage *destination=(owner==next->physical_index?
            next->commands:next->command_peer)+index;
        memcpy(destination,bank_command_const(active,index),sizeof(*destination));
        ksn_p0_probe_copy(KSN_P0_CORE_CLONE_COMMAND,sizeof(*destination));
        select_command_owner(next,index,owner);
    }
    return bank_command(next,index);
}
static uint8_t *bank_text(ksn_bank *bank,ksn_layer layer,unsigned offset){
    return bank->text_layer[layer]+offset-text_base(layer);
}
static const uint8_t *bank_text_const(const ksn_bank *bank,ksn_layer layer,unsigned offset){
    return bank->text_layer[layer]+offset-text_base(layer);
}
static unsigned track_base(ksn_layer layer){return layer==KSN_APP?0:KSN_APP_TRACKS;}
static unsigned track_limit(ksn_layer layer){return layer==KSN_APP?KSN_APP_TRACKS:KSN_SYSTEM_TRACKS;}
static unsigned track_owner(const ksn_bank *bank,unsigned index){return (bank->track_owner>>index)&1u;}
static void select_track_owner(ksn_bank *bank,unsigned index,unsigned owner){
    uint8_t bit=(uint8_t)(1u<<index);
    if(owner)bank->track_owner|=bit;else bank->track_owner&=(uint8_t)~bit;
}
static ksn_track *bank_track(ksn_bank *bank,unsigned index){
    return (track_owner(bank,index)==bank->physical_index?bank->tracks:bank->track_peer)+index;
}
static const ksn_track *bank_track_const(const ksn_bank *bank,unsigned index){
    return (track_owner(bank,index)==bank->physical_index?bank->tracks:bank->track_peer)+index;
}
static ksn_track *private_track(ksn_core_impl *core,unsigned index){
    ksn_bank *next=&core->banks[core->building_bank];
    const ksn_bank *active=&core->banks[core->active];
    if(track_owner(next,index)==track_owner(active,index)){
        unsigned owner=track_owner(active,index)^1u;
        select_track_owner(next,index,owner);
        ksn_track *destination=bank_track(next,index);
        memcpy(destination,bank_track_const(active,index),sizeof(*destination));
        ksn_p0_probe_copy(KSN_P0_CORE_CLONE_TRACK,sizeof(*destination));
    }
    return bank_track(next,index);
}
/* A new animation assigns the entire slot, so its prior value need not be
 * copied even when this PATCH is the first writer of that slot. */
static ksn_track *fresh_track(ksn_core_impl *core,unsigned index){
    ksn_bank *next=&core->banks[core->building_bank];
    const ksn_bank *active=&core->banks[core->active];
    if(track_owner(next,index)==track_owner(active,index))
        select_track_owner(next,index,track_owner(active,index)^1u);
    return bank_track(next,index);
}
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
    ksn_p0_probe_copy(KSN_P0_CORE_PAYLOAD_WRITE,bytes);
}
static void payload_read(const ksn_command_storage *command,void *payload,size_t bytes){
    memcpy(payload,command->payload,bytes);
    ksn_p0_probe_copy(KSN_P0_CORE_PAYLOAD_READ,bytes);
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

/* Only the published prefix of each layer can be read. Every new command is
 * assigned in full by add(), and every new text allocation is initialized by
 * add()/change(), so stale bytes beyond count/text_used need not cross banks.
 * Animation slots are independently borrowed and privatized on write. */
static void copy_live_text(ksn_bank *next,const ksn_bank *active,ksn_layer layer,
                           unsigned start,unsigned end){
    if(start>=end)return;
    size_t bytes=end-start;
    memcpy(bank_text(next,layer,start),bank_text_const(active,layer,start),bytes);
    ksn_p0_probe_copy(KSN_P0_CORE_CLONE_TEXT,bytes);
}
static bool text_slot_written(const ksn_core_impl *core,unsigned index){
    return (core->text_written[index/32u]&(1u<<(index%32u)))!=0;
}
static void mark_text_slot_written(ksn_core_impl *core,unsigned index){
    core->text_written[index/32u]|=1u<<(index%32u);
    core->text_written_layers|=(uint8_t)(1u<<core->layer);
}
static void privatize_text_layer(ksn_core_impl *core,ksn_layer layer){
    ksn_bank *next=&core->banks[core->building_bank];
    const ksn_bank *active=&core->banks[core->active];
    if(next->text_layer[layer]!=active->text_layer[layer])return;
    unsigned base=text_base(layer);
    next->text_layer[layer]=active->text_layer[layer]==core->banks[0].text+base?
        core->banks[1].text+base:core->banks[0].text+base;
}
/* Text allocations are ordered by add(), so changed slots divide the live
 * prefix into contiguous unchanged spans. No old bytes of a changed slot
 * cross banks, even if that slot is rewritten more than once in one PATCH. */
static void copy_patch_text_layer(ksn_core_impl *core,ksn_layer layer){
    ksn_bank *next=&core->banks[core->building_bank];
    const ksn_bank *active=&core->banks[core->active];
    if(next->text_layer[layer]==active->text_layer[layer])return;
    unsigned cursor=text_base(layer),end=cursor+active->text_used[layer];
    if(!(core->text_written_layers&(1u<<layer))){
        copy_live_text(next,active,layer,cursor,end);return;
    }
    unsigned base=command_base(layer);
    for(unsigned i=0;i<active->count[layer];i++){
        unsigned index=base+i;
        if(!text_slot_written(core,index))continue;
        const ksn_command_storage *command=bank_command_const(active,index);
        if(command->kind!=KSN_TEXT)continue;
        text_payload payload;payload_read(command,&payload,sizeof(payload));
        copy_live_text(next,active,layer,cursor,payload.offset);
        cursor=(unsigned)payload.offset+payload.capacity;
    }
    copy_live_text(next,active,layer,cursor,end);
}

/* Candidate identity (physical blocks and index) is fixed by core_bind.
 * Copy only the logical metadata that belongs to the sealed scene. A whole
 * ksn_bank assignment needlessly copied the physical pointers and padding,
 * then rewrote them with the same candidate-owned values on every begin. */
static void clone_bank_metadata(ksn_bank *next,const ksn_bank *active){
    memcpy(next->command_owner,active->command_owner,sizeof(next->command_owner));
    memcpy(next->text_layer,active->text_layer,sizeof(next->text_layer));
    next->track_owner=active->track_owner;
    memcpy(next->count,active->count,sizeof(next->count));
    memcpy(next->text_used,active->text_used,sizeof(next->text_used));
    memcpy(next->generation,active->generation,sizeof(next->generation));
    memcpy(next->background,active->background,sizeof(next->background));
    memcpy(next->background_set,active->background_set,sizeof(next->background_set));
    ksn_p0_probe_copy(KSN_P0_CORE_CLONE_META,
        sizeof(next->command_owner)+sizeof(next->text_layer)+
        sizeof(next->track_owner)+sizeof(next->count)+sizeof(next->text_used)+
        sizeof(next->generation)+sizeof(next->background)+
        sizeof(next->background_set));
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
    clone_bank_metadata(next,active);
    /* PATCH borrows both immutable layers until its first text write. REPLACE
     * needs a private edited layer immediately; the other layer stays shared.
     * A failed or pending frame never writes the active region. */
    if(mode==KSN_REPLACE)privatize_text_layer(core,layer);
    /* PATCH borrows every track. REPLACE clears only the edited layer's
     * opposite physical slots; the other layer remains sealed and borrowed. */
    memset(core->text_written,0,sizeof(core->text_written));
    core->text_written_layers=0;
    core->layer=layer;core->mode=mode;core->poison=KSN_OK;core->building=true;
    core->transaction=(ksn_tx){++last_transaction};*out=core->transaction;
    if(mode==KSN_REPLACE){
        uint32_t generation=++last_generation;
        ksn_bank *bank=&core->banks[core->building_bank];
        bank->generation[layer]=generation;bank->count[layer]=0;bank->text_used[layer]=0;
        bank->background_set[layer]=false;
        memset(bank_text(bank,layer,text_base(layer)),0,text_limit(layer));
        if(bank->tracks)for(unsigned i=track_base(layer);i<track_base(layer)+track_limit(layer);i++){
            select_track_owner(bank,i,track_owner(active,i)^1u);
            memset(bank_track(bank,i),0,sizeof(ksn_track));
        }
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
ksn_result ksn_core_check_draw(const ksn_core *storage,ksn_layer layer,
                               const ksn_draw *draw){
    if(!storage||!valid_layer(layer))return KSN_INVALID;
    ksn_result result=validate_draw(draw);
    if(result!=KSN_OK||draw->kind!=KSN_IMAGE)return result;
    const ksn_core_impl *core=cimpl(storage);
    result=validate_image(core,layer,draw->data.image.resource,
                          draw->data.image.variant,draw->data.image.frame);
    if(result!=KSN_OK)return result;
    return validate_image_window(core,layer,draw->data.image.resource,draw->bounds,
                                 draw->data.image.source_x,draw->data.image.source_y,
                                 draw->data.image.scale,draw->data.image.source_width,
                                 draw->data.image.source_height);
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
        memset(bank_text(bank,layer,offset),0,capacity);
        if(draw->data.text.bytes){memcpy(bank_text(bank,layer,offset),draw->data.text.utf8,draw->data.text.bytes);
            ksn_p0_probe_copy(KSN_P0_CORE_SUBMIT_TEXT,draw->data.text.bytes);
            ksn_p0_probe_core_source_text(draw->data.text.utf8,draw->data.text.bytes);}
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
    select_command_owner(bank,index,command_owner(&core->banks[core->active],index)^1u);
    *bank_command(bank,index)=command;bank->count[layer]++;
    ksn_p0_probe_copy(KSN_P0_CORE_SUBMIT_COMMAND,sizeof(command));
    *out=make_ref(bank->generation[layer],index);return KSN_OK;
}
static ksn_result referenced_command(ksn_core_impl *core,ksn_ref ref,ksn_command_storage **out){
    unsigned index=ref_index(ref),base=command_base(core->layer),limit=command_limit(core->layer);
    ksn_bank *bank=&core->banks[core->building_bank];
    if(index<base||index>=base+limit||index>=base+bank->count[core->layer]||
       ref_generation(ref)!=bank->generation[core->layer])return KSN_STALE;
    *out=private_command(core,index);return KSN_OK;
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
            if(running(bank_track_const(&core->banks[core->building_bank],i))&&
               bank_track_const(&core->banks[core->building_bank],i)->target.value==ref.value)
                return poison(core,KSN_BUSY);
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
        if(core->mode==KSN_PATCH)privatize_text_layer(core,core->layer);
        memset(bank_text(bank,core->layer,p.offset),0,p.capacity);
        if(change->value.text.bytes){memcpy(bank_text(bank,core->layer,p.offset),change->value.text.utf8,change->value.text.bytes);
            ksn_p0_probe_copy(KSN_P0_CORE_SUBMIT_TEXT,change->value.text.bytes);
            ksn_p0_probe_core_source_text(change->value.text.utf8,change->value.text.bytes);}
        p.length=(uint8_t)change->value.text.bytes;p.reveal=(uint8_t)count;payload_write(command,&p,sizeof(p));
        if(core->mode==KSN_PATCH)mark_text_slot_written(core,ref_index(ref));
        return KSN_OK;
    }
    case KSN_SET_REVEAL:{
        if(command->kind!=KSN_TEXT)return poison(core,KSN_INVALID);
        text_payload p;payload_read(command,&p,sizeof(p));
        const ksn_bank *bank=&core->banks[core->mode==KSN_PATCH&&!text_slot_written(core,ref_index(ref))
                                                 ?core->active:core->building_bank];
        size_t count=utf8_count((const char *)bank_text_const(bank,core->layer,p.offset),p.length);
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
static int find_track_index(const ksn_bank *bank,ksn_layer layer,ksn_animation id){
    if(!bank->tracks||!id.value)return -1;
    for(unsigned i=track_base(layer);i<track_base(layer)+track_limit(layer);i++)
        if(bank_track_const(bank,i)->id.value==id.value)return (int)i;
    return -1;
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
    int slot=-1;
    for(unsigned i=track_base(core->layer);i<track_base(core->layer)+track_limit(core->layer);i++){
        const ksn_track *t=bank_track_const(bank,i);
        if(running(t)&&t->target.value==motion->first.value)return poison(core,KSN_BUSY);
        if(!running(t)&&slot<0)slot=(int)i;
    }
    if(slot<0||last_animation==UINT32_MAX)return poison(core,KSN_LIMIT);
    ksn_track *written=fresh_track(core,(unsigned)slot);
    *written=(ksn_track){.from=motion->from.pose,.to=motion->to.pose,.id={++last_animation},.target=motion->first,
        .duration_ms=motion->duration_ms,.easing=(uint8_t)motion->easing,.repeat=(uint8_t)motion->repeat,
        .status=KSN_ANIMATION_PENDING};
    apply_pose(command,written->from);*out=written->id;return KSN_OK;
}
static ksn_result core_stop(void *context,ksn_tx tx,ksn_animation animation){
    ksn_core_impl *core=((ksn_endpoint *)context)->core;ksn_result result=check_transaction(context,tx);
    if(result!=KSN_OK)return result;
    int index=find_track_index(&core->banks[core->building_bank],core->layer,animation);
    if(index<0)return poison(core,KSN_STALE);
    if(running(bank_track_const(&core->banks[core->building_bank],(unsigned)index)))
        private_track(core,(unsigned)index)->status=KSN_ANIMATION_STOPPED;
    return KSN_OK;
}
static void sample_tracks(ksn_core_impl *core,uint64_t now,bool reduce);
static ksn_result core_end(void *context,ksn_tx tx){
    ksn_core_impl *core=((ksn_endpoint *)context)->core;ksn_result result=check_transaction(context,tx);
    if(result!=KSN_OK)return result;
    if(!core->banks[core->building_bank].background_set[KSN_APP])return poison(core,KSN_INVALID);
    if(core->mode==KSN_PATCH){
        copy_patch_text_layer(core,KSN_APP);
        copy_patch_text_layer(core,KSN_SYSTEM);
    }
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
    if(bank->tracks)for(unsigned i=track_base(layer);i<track_base(layer)+track_limit(layer);i++)
        count+=running(bank_track_const(bank,i));
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
        core->banks[i].commands=commands[i];core->banks[i].command_peer=commands[i^1u];
        core->banks[i].physical_index=(uint8_t)i;
        if(i)for(unsigned word=0;word<(KSN_COMMANDS+31u)/32u;word++)
            core->banks[i].command_owner[word]=UINT32_MAX;
        core->banks[i].text=text[i];
        for(unsigned layer=0;layer<2;layer++)
            core->banks[i].text_layer[layer]=text[i]+text_base((ksn_layer)layer);
        core->banks[i].tracks=tracks[i];core->banks[i].track_peer=tracks[i^1u];
        core->banks[i].track_owner=i?UINT8_MAX:0;
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
        for(unsigned index=command_base(layer);index<command_base(layer)+command_limit(layer);index++)
            select_command_owner(bank,index,i);
        memset(bank->text+text_base(layer),0,text_limit(layer));
        bank->count[layer]=bank->text_used[layer]=0;bank->generation[layer]=0;
        if(bank->tracks)for(unsigned index=track_base(layer);index<track_base(layer)+track_limit(layer);index++){
            memset(bank->tracks+index,0,sizeof(ksn_track));
            select_track_owner(bank,index,i);
        }
        bank->background[layer]=layer==KSN_APP?0x000000ff:0;
        bank->background_set[layer]=layer==KSN_APP;
    }
    unsigned kept=0;
    for(unsigned i=0;i<core->image_count;i++)
        if(core->images[i].layer!=layer)core->images[kept++]=core->images[i];
    memset(core->images+kept,0,(core->image_count-kept)*sizeof(*core->images));
    core->image_count=(uint8_t)kept;core->invalidated=KSN_BANDS_ALL;
    return KSN_OK;
}
ksn_result ksn_core_enable_animation(ksn_core *storage,ksn_core_animation_block *a,ksn_core_animation_block *b){
    if(!storage||!a||!b||a==b)return KSN_INVALID;
    ksn_core_impl *core=impl(storage);
    if(core->submitted||core->repairing||core->banks[0].tracks||core->banks[1].tracks)return KSN_BUSY;
    memset(a,0,sizeof(*a));memset(b,0,sizeof(*b));
    core->banks[0].tracks=a->tracks;core->banks[0].track_peer=b->tracks;
    core->banks[1].tracks=b->tracks;core->banks[1].track_peer=a->tracks;
    core->banks[0].track_owner=0;core->banks[1].track_owner=UINT8_MAX;
    return KSN_OK;
}
uint32_t ksn_core_animation_bytes(const ksn_core *storage){
    return storage&&cimpl(storage)->banks[0].tracks?2*sizeof(ksn_core_animation_block):0;
}
ksn_result ksn_core_finish_animation(ksn_core *storage,ksn_layer layer,ksn_tx tx,ksn_animation id){
    if(!storage||!valid_layer(layer))return KSN_INVALID;
    ksn_core_impl *core=impl(storage);ksn_result r=check_transaction(&core->endpoints[layer],tx);
    if(r!=KSN_OK)return r;
    int index=find_track_index(&core->banks[core->building_bank],layer,id);
    if(index<0)return poison(core,KSN_STALE);
    const ksn_track *current=bank_track_const(&core->banks[core->building_bank],(unsigned)index);
    ksn_command_storage *command;r=referenced_command(core,current->target,&command);
    if(r!=KSN_OK)return poison(core,r);
    ksn_track *t=private_track(core,(unsigned)index);
    apply_pose(command,t->to);t->status=KSN_ANIMATION_FINISHED;return KSN_OK;
}
ksn_animation_status ksn_core_poll_animation(const ksn_core *storage,ksn_layer layer,ksn_animation id){
    if(!storage||!valid_layer(layer)||!id.value)return KSN_ANIMATION_DISCARDED;
    const ksn_core_impl *core=cimpl(storage);const ksn_bank *active=&core->banks[core->active];
    if(active->tracks)for(unsigned i=track_base(layer);i<track_base(layer)+track_limit(layer);i++)
        if(bank_track_const(active,i)->id.value==id.value)
            return (ksn_animation_status)bank_track_const(active,i)->status;
    const ksn_bank *next=&core->banks[core->building_bank];
    if((core->building||core->submitted)&&next->tracks)
        for(unsigned i=track_base(layer);i<track_base(layer)+track_limit(layer);i++)
            if(bank_track_const(next,i)->id.value==id.value)return KSN_ANIMATION_PENDING;
    return KSN_ANIMATION_DISCARDED;
}
void ksn_core_start_animations(ksn_core *storage,uint64_t now){
    if(!storage)return;
    ksn_core_impl *core=impl(storage);
    core->animation_now_us=now;
    if(core->building||core->submitted||core->repairing)return;
    ksn_bank *active=&core->banks[core->active];if(!active->tracks)return;
    for(unsigned i=0;i<KSN_TRACKS;i++)if(bank_track_const(active,i)->status==KSN_ANIMATION_PENDING){
        ksn_track *t=bank_track(active,i);
        t->started_us=t->sampled_us=now;t->status=KSN_ANIMATION_RUNNING;
    }
}
uint64_t ksn_core_animation_deadline(const ksn_core *storage){
    if(!storage)return UINT64_MAX;
    const ksn_bank *active=&cimpl(storage)->banks[cimpl(storage)->active];
    uint64_t next=UINT64_MAX;if(!active->tracks)return next;
    for(unsigned i=0;i<KSN_TRACKS;i++){
        const ksn_track *t=bank_track_const(active,i);
        if(t->status!=KSN_ANIMATION_RUNNING)continue;
        uint64_t due=t->sampled_us>UINT64_MAX-33334?UINT64_MAX:t->sampled_us+33334;
        if(t->easing==KSN_STEP&&t->repeat==KSN_ONCE){
            uint64_t duration=(uint64_t)t->duration_ms*1000;
            uint64_t end=t->started_us>UINT64_MAX-duration?UINT64_MAX:t->started_us+duration;
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
        const ksn_track *current=bank_track_const(bank,i);
        if(current->status!=KSN_ANIMATION_RUNNING)continue;
        if(!reduce&&(now<current->sampled_us||now-current->sampled_us<33334))continue;
        ksn_track *t=private_track(core,i);
        uint64_t elapsed=now>=t->started_us?now-t->started_us:0;bool done;
        uint32_t q=motion_progress(t,elapsed,reduce,&done);
        ksn_pose p={.bounds={motion_lerp(t->from.bounds.x0,t->to.bounds.x0,q),motion_lerp(t->from.bounds.y0,t->to.bounds.y0,q),
            motion_lerp(t->from.bounds.x1,t->to.bounds.x1,q),motion_lerp(t->from.bounds.y1,t->to.bounds.y1,q)},
            .rotation=motion_lerp(t->from.rotation,t->to.rotation,q)};
        apply_pose(private_command(core,ref_index(t->target)),p);t->sampled_us=now;
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
    return storage&&(cimpl(storage)->full_redraw||cimpl(storage)->invalidated||
                     cimpl(storage)->repair_bands);
}
void ksn_core_invalidate(ksn_core *storage){ksn_core_invalidate_bands(storage,KSN_BANDS_ALL);}
void ksn_core_invalidate_bands(ksn_core *storage,uint32_t bands){
    if(storage)impl(storage)->invalidated|=bands&KSN_BANDS_ALL;
}
uint32_t ksn_core_opaque_system_bands(const ksn_core *storage){
    if(!storage)return 0;
    const ksn_core_impl *core=cimpl(storage);
    /* A pending SYSTEM replacement may expose the old backdrop. Repair also
     * cannot assume that the glass contains the committed opaque pixels. */
    if(core->repairing||core->full_redraw||core->repair_bands||
       (core->submitted&&core->layer==KSN_SYSTEM))return 0;
    const ksn_bank *bank=&core->banks[core->active];
    uint32_t covered=0;
    for(unsigned i=0;i<bank->count[KSN_SYSTEM];i++){
        const ksn_command_storage *command=bank_command_const(bank,KSN_APP_COMMANDS+i);
        if(command->kind!=KSN_RECT||command->opacity!=255||
           !(command->flags&KSN_FLAG_VISIBLE)||(command->flags&KSN_FLAG_GROUP)||
           command->bounds.x0>0||command->bounds.x1<240||
           command->clip.x0>0||command->clip.x1<240)continue;
        ksn_rgba color;
        memcpy(&color,command->payload,sizeof(color));
        if((color&255u)!=255u)continue;
        for(unsigned band=0;band<17;band++){
            int y0=(int)band*8,y1=y0+8;
            if(y1>135)y1=135;
            if(command->bounds.y0<=y0&&command->bounds.y1>=y1&&
               command->clip.y0<=y0&&command->clip.y1>=y1)
                covered|=1u<<band;
        }
    }
    return covered;
}
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
    ksn_bank *bank=&core->banks[core->building_bank];
    for(unsigned i=0;i<count;i++){
        unsigned expected=KSN_FLAG_GROUP|(i==0?KSN_FLAG_GROUP_BEGIN:0)|(i+1==count?KSN_FLAG_GROUP_END:0);
        const ksn_command_storage *part=bank_command_const(bank,ref_index(first)+i);
        unsigned flags=part->flags&(KSN_FLAG_GROUP|KSN_FLAG_GROUP_BEGIN|KSN_FLAG_GROUP_END);
        if(flags!=(existing?expected:0))return poison(core,KSN_INVALID);
    }
    for(unsigned i=0;i<count;i++){
        ksn_command_storage *part=private_command(core,ref_index(first)+i);
        part->flags|=KSN_FLAG_GROUP|(i==0?KSN_FLAG_GROUP_BEGIN:0)|(i+1==count?KSN_FLAG_GROUP_END:0);
        part->reserved=opacity;
    }
    return KSN_OK;
}
ksn_result ksn_core_presented(ksn_core *storage,ksn_tx ticket){
    if(!storage)return KSN_INVALID;
    ksn_core_impl *core=impl(storage);
    if((!core->submitted&&!core->repairing)||ticket.value!=core->transaction.value)return KSN_STALE;
    if(core->repairing){
        core->repairing=false;core->full_redraw=false;core->repair_bands=0;return KSN_OK;
    }
    core->active=core->building_bank;core->submitted=false;core->full_redraw=false;
    core->repair_bands=0;
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
                    .full_redraw=core->full_redraw||core->invalidated==KSN_BANDS_ALL};
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
     * the current acknowledgement. IO failure keeps full_redraw set too.
     * Only an unqualified invalidate becomes full_redraw; a banded one is owed
     * by repair_bands, which damage seeds and present clears. */
    if(core->invalidated==KSN_BANDS_ALL)core->full_redraw=true;
    core->repair_bands|=core->invalidated;core->invalidated=0;
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
static ksn_result read_command(const ksn_bank *bank,ksn_layer layer,
                               uint16_t index,ksn_frame_command *out,bool borrow_text){
    if(index>=bank->count[layer])return KSN_INVALID;
    const ksn_command_storage *command=bank_command_const(bank,command_base(layer)+index);
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
        if(borrow_text)draw->data.text.utf8=(const char *)bank_text_const(bank,layer,p.offset);
        else{
            memcpy(out->text,bank_text_const(bank,layer,p.offset),p.length);
            ksn_p0_probe_copy(KSN_P0_CORE_RENDER_TEXT,p.length);
            draw->data.text.utf8=out->text;
        }
        draw->data.text.bytes=p.length;
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

ksn_result ksn_core_read(const ksn_core *storage,ksn_tx ticket,bool previous,
                       ksn_layer layer,uint16_t index,ksn_frame_command *out){
    if(!storage||!out||!valid_layer(layer))return KSN_INVALID;
    const ksn_core_impl *core=cimpl(storage);
    if((!core->submitted&&!core->repairing)||ticket.value!=core->transaction.value)return KSN_STALE;
    const ksn_bank *bank=&core->banks[(previous||core->repairing)?core->active:core->building_bank];
    return read_command(bank,layer,index,out,false);
}

ksn_result ksn_core_read_borrowed(const ksn_core *storage,ksn_tx ticket,bool previous,
                                ksn_layer layer,uint16_t index,ksn_frame_command *out){
    if(!storage||!out||!valid_layer(layer))return KSN_INVALID;
    const ksn_core_impl *core=cimpl(storage);
    if((!core->submitted&&!core->repairing)||ticket.value!=core->transaction.value)return KSN_STALE;
    const ksn_bank *bank=&core->banks[(previous||core->repairing)?core->active:core->building_bank];
    return read_command(bank,layer,index,out,true);
}

ksn_result ksn_core_read_active_ref(const ksn_core *storage,ksn_layer layer,
                                    ksn_ref ref,ksn_frame_command *out){
    if(!storage||!out||!ref.value||!valid_layer(layer))return KSN_INVALID;
    const ksn_core_impl *core=cimpl(storage);
    const ksn_bank *bank=&core->banks[core->active];
    unsigned index=ref_index(ref),base=command_base(layer);
    if(index<base||index>=base+bank->count[layer]||
       ref_generation(ref)!=bank->generation[layer])return KSN_STALE;
    return read_command(bank,layer,(uint16_t)(index-base),out,true);
}

ksn_result ksn_core_image_span(const ksn_core *storage,ksn_tx ticket,bool previous,
                            ksn_layer layer,uint16_t index,uint16_t y,uint16_t x,
                            uint16_t count,uint16_t *rgb565,uint8_t *alpha){
    if(!storage||!valid_layer(layer))return KSN_INVALID;
    const ksn_core_impl *core=cimpl(storage);
    if((!core->submitted&&!core->repairing)||ticket.value!=core->transaction.value)return KSN_STALE;
    const ksn_bank *bank=&core->banks[(previous||core->repairing)?core->active:core->building_bank];
    if(index>=bank->count[layer])return KSN_INVALID;
    const ksn_command_storage *command=bank_command_const(bank,command_base(layer)+index);
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

/* The command's clipped box on the panel, or an empty rect when it paints
 * nothing. Both sides of the diff go through this, so a command that moved
 * contributes the union of where it was and where it is. */
static ksn_rect command_box(const ksn_command_storage *command){
    const ksn_rect empty={0,0,0,0};
    if(!(command->flags&KSN_FLAG_VISIBLE)||!command->opacity)return empty;
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
    if(x0>=x1||y0>=y1)return empty;
    return (ksn_rect){(int16_t)x0,(int16_t)y0,(int16_t)x1,(int16_t)y1};
}
/* Union a box into the damage, per band. A band's columns are the union of the
 * boxes that touch it, which is why a change low on the panel cannot widen a
 * band it does not reach. */
static void damage_add(ksn_damage *d,ksn_rect box){
    if(box.x0>=box.x1||box.y0>=box.y1)return;
    for(int band=box.y0/8;band<=(box.y1-1)/8;band++){
        uint32_t bit=1u<<band;
        if(!(d->bands&bit)){
            d->bands|=bit;d->x0[band]=box.x0;d->x1[band]=box.x1;
        }else{
            if(box.x0<d->x0[band])d->x0[band]=box.x0;
            if(box.x1>d->x1[band])d->x1[band]=box.x1;
        }
    }
}
/* One scalar out of text the bank has already validated, so the checks
 * utf8_count makes on the way in are not repeated here. Returns 0 and consumes
 * one byte on anything malformed, which cannot happen and would only cost a
 * wider damage box if it did. */
static uint32_t utf8_next(const uint8_t *text,size_t bytes,size_t *at){
    uint8_t a=text[(*at)++];unsigned more;uint32_t cp;
    if(a<0x80u)return a;
    else if(a>=0xc2u&&a<=0xdfu){cp=a&0x1fu;more=1;}
    else if(a>=0xe0u&&a<=0xefu){cp=a&0x0fu;more=2;}
    else if(a>=0xf0u&&a<=0xf4u){cp=a&7u;more=3;}
    else return 0;
    if(*at+more>bytes){*at=bytes;return 0;}
    for(unsigned n=0;n<more;n++)cp=(cp<<6)|(text[(*at)++]&0x3fu);
    return cp;
}
/* How wide this run actually draws: the advances of the scalars the reveal
 * lets through, which is bounded by the CONTENT rather than by the declared
 * box. A text command is usually given a box with room to grow, so this is the
 * difference between dirtying what is written and dirtying what was reserved. */
static unsigned run_width(const ksn_text_port *text,ksn_font font,
                          const uint8_t *s,size_t bytes,unsigned reveal){
    size_t at=0;unsigned pen=0,scalar=0;
    while(at<bytes&&scalar<reveal){
        pen+=text->advance(text->ctx,font,utf8_next(s,bytes,&at));scalar++;
    }
    return pen;
}
/* The columns of a text command that actually changed.
 *
 * A counter that goes from "KEY PRESSES: 5" to "KEY PRESSES: 6" redraws one
 * caption glyph, but the diff above only knows the bytes differ, so the command
 * dirtied its whole declared box -- 184 pixels for six. This walks the two runs
 * scalar by scalar in lockstep, accumulating the pen, and returns the span from
 * the first scalar that differs to the end of the last one.
 *
 * The tight answer needs the walk to stay in lockstep: equal advances at every
 * position and the same number of scalars. One scalar of a different width, or
 * one run longer than the other, and every glyph to the right of that point has
 * MOVED -- "9" becoming "10" shifts nothing here but would in a run with a
 * following word. So the answer from there rightwards is the wider of the two
 * runs' own widths, which is still the content and not the box: 12 columns for
 * that counter instead of 184.
 *
 * Scalars past BOTH reveals are not drawn, so a difference there is not a
 * difference on the panel -- which also means a setText that only rewrites
 * hidden text yields no damage at all, and a run that diverges only past both
 * reveals reports an empty range.
 *
 * `reveal` is a scalar count, so it is compared against the scalar index; the
 * two runs share a bank offset and a font, which the caller has checked. */
static bool text_changed_columns(const ksn_text_port *text,ksn_font font,
                                 const uint8_t *a,size_t a_bytes,unsigned a_reveal,
                                 const uint8_t *b,size_t b_bytes,unsigned b_reveal,
                                 int *first,int *last){
    if(!text||!text->advance)return false;
    size_t at_a=0,at_b=0;unsigned scalar=0,pen=0;
    int lo=-1,hi=0;
    bool lockstep=true;
    while(at_a<a_bytes&&at_b<b_bytes){
        size_t was_a=at_a,was_b=at_b;
        uint32_t cp_a=utf8_next(a,a_bytes,&at_a),cp_b=utf8_next(b,b_bytes,&at_b);
        unsigned adv=text->advance(text->ctx,font,cp_a);
        if(adv!=text->advance(text->ctx,font,cp_b)){
            at_a=was_a;at_b=was_b;lockstep=false;break;
        }
        bool vis_a=scalar<a_reveal,vis_b=scalar<b_reveal;
        if((vis_a||vis_b)&&(cp_a!=cp_b||vis_a!=vis_b)){
            if(lo<0)lo=(int)pen;
            hi=(int)(pen+adv);
        }
        pen+=adv;scalar++;
    }
    if(lockstep&&at_a==a_bytes&&at_b==b_bytes){
        if(lo<0){*first=*last=0;return true;} /* nothing visible moved */
        *first=lo;*last=hi;return true;
    }
    /* Diverged. Everything from here to the end of the longer drawn run. */
    if(lo<0)lo=(int)pen;
    unsigned wide_a=run_width(text,font,a,a_bytes,a_reveal);
    unsigned wide_b=run_width(text,font,b,b_bytes,b_reveal);
    hi=(int)(wide_a>wide_b?wide_a:wide_b);
    if(hi<=lo){*first=*last=0;return true;}
    *first=lo;*last=hi;return true;
}
/* Bands whose columns nobody described: the whole width. */
static void damage_widen(ksn_damage *d,uint32_t bands){
    for(int band=0;band<17;band++){
        if(!(bands&(1u<<band)))continue;
        if(!(d->bands&(1u<<band))){d->bands|=1u<<band;d->x0[band]=0;d->x1[band]=240;}
        else{d->x0[band]=0;d->x1[band]=240;}
    }
}
/* True when the two sides are the same text command differing only in what it
 * says -- same box, same clip, same font, same colour, same bank offset. Only
 * then is a column range meaningful: anything else moved the box itself. */
static bool same_text_frame(const ksn_command_storage *a,const ksn_command_storage *b,
                            const text_payload *pa,const text_payload *pb){
    return a->kind==KSN_TEXT&&b->kind==KSN_TEXT&&a->flags==b->flags&&
           a->opacity==b->opacity&&a->reserved==b->reserved&&
           a->bounds.x0==b->bounds.x0&&a->bounds.y0==b->bounds.y0&&
           a->bounds.x1==b->bounds.x1&&a->bounds.y1==b->bounds.y1&&
           a->clip.x0==b->clip.x0&&a->clip.y0==b->clip.y0&&
           a->clip.x1==b->clip.x1&&a->clip.y1==b->clip.y1&&
           pa->offset==pb->offset&&pa->capacity==pb->capacity&&pa->font==pb->font&&
           pa->flags==pb->flags&&pa->color==pb->color;
}
ksn_result ksn_core_damage(const ksn_core *storage,ksn_tx ticket,
                           const ksn_text_port *text,ksn_damage *out){
    if(!storage||!out)return KSN_INVALID;
    const ksn_core_impl *core=cimpl(storage);
    if((!core->submitted&&!core->repairing)||ticket.value!=core->transaction.value)return KSN_STALE;
    const ksn_bank *old=&core->banks[core->active],*next=&core->banks[core->building_bank];
    memset(out,0,sizeof(*out));
    if(core->full_redraw||old->background[KSN_APP]!=next->background[KSN_APP]||
       old->generation[0]!=next->generation[0]||old->generation[1]!=next->generation[1]){
        damage_widen(out,KSN_BANDS_ALL);return KSN_OK;
    }
    for(unsigned layer=0;layer<2;layer++)for(unsigned i=0;i<next->count[layer];i++){
        unsigned index=command_base((ksn_layer)layer)+i;
        const ksn_command_storage *a=bank_command_const(old,index),*b=bank_command_const(next,index);
        bool changed=memcmp(a,b,sizeof(*a))!=0;
        if(!changed&&b->kind==KSN_TEXT){
            text_payload p;payload_read(b,&p,sizeof(p));
            changed=memcmp(bank_text_const(old,(ksn_layer)layer,p.offset),
                           bank_text_const(next,(ksn_layer)layer,p.offset),p.length)!=0;
        }
        if(!changed)continue;
        /* A text command that only changed what it says contributes the columns
         * of the scalars that differ, not its whole declared box. */
        if(a->kind==KSN_TEXT&&b->kind==KSN_TEXT){
            text_payload pa,pb;payload_read(a,&pa,sizeof(pa));payload_read(b,&pb,sizeof(pb));
            int first,last;
            if(same_text_frame(a,b,&pa,&pb)&&
               text_changed_columns(text,(ksn_font)pa.font,
                                    bank_text_const(old,(ksn_layer)layer,pa.offset),pa.length,pa.reveal,
                                    bank_text_const(next,(ksn_layer)layer,pb.offset),pb.length,pb.reveal,
                                    &first,&last)){
                if(first==last)continue; /* nothing visible moved */
                ksn_rect box=command_box(b);
                if(box.x0<box.x1){
                    int x0=b->bounds.x0+first,x1=b->bounds.x0+last;
                    if(x0<box.x0)x0=box.x0;
                    if(x1>box.x1)x1=box.x1;
                    if(x0<x1)damage_add(out,(ksn_rect){(int16_t)x0,box.y0,(int16_t)x1,box.y1});
                }
                continue;
            }
        }
        damage_add(out,command_box(a));damage_add(out,command_box(b));
    }
    /* What a repair already owes, added last because an owner that asked for a
     * band did not say which columns, and full width wins over any range the
     * diff put there. A repairing frame reads the same bank on both sides, so
     * the loop above found nothing and these are the only bands it has. */
    damage_widen(out,core->repair_bands);
    return KSN_OK;
}
