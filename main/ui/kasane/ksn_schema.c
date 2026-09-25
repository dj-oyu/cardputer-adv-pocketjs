#include "ksn_schema.h"
#include "ksn_core.h"
#include <string.h>

static bool text_valid(ksn_schema_text text,uint8_t capacity){
    if((text.bytes&&!text.utf8)||text.bytes>capacity)return false;
    for(unsigned i=0;i<text.bytes;){
        uint8_t lead=(uint8_t)text.utf8[i];
        unsigned n=lead<0x80?1:lead>=0xc2&&lead<=0xdf?2:
                   lead>=0xe0&&lead<=0xef?3:lead>=0xf0&&lead<=0xf4?4:0;
        if(!n||i+n>text.bytes)return false;
        uint32_t cp=n==1?lead:lead&((1u<<(7u-n))-1u);
        for(unsigned j=1;j<n;j++){
            uint8_t b=(uint8_t)text.utf8[i+j];
            if((b&0xc0u)!=0x80u)return false;
            cp=(cp<<6)|(b&0x3fu);
        }
        if(cp<0x20u||cp==0x7fu||(n==2&&cp<0x80u)||
           (n==3&&(cp<0x800u||(cp>=0xd800u&&cp<=0xdfffu)))||
           (n==4&&(cp<0x10000u||cp>0x10ffffu)))return false;
        i+=n;
    }
    return true;
}

static bool binding_valid(const ksn_schema *schema,const ksn_schema_binding *binding,
                          ksn_slot_type type){
    if(binding->slot==KSN_SCHEMA_LITERAL){
        if(type==KSN_SLOT_TEXT)return text_valid(binding->literal.text,KSN_SCHEMA_TEXT_MAX);
        if(type==KSN_SLOT_RECT){ksn_rect r=binding->literal.rect;return r.x0<=r.x1&&r.y0<=r.y1;}
        return true;
    }
    return binding->slot<schema->slot_count&&schema->slots[binding->slot].type==type;
}

ksn_result ksn_schema_validate(const ksn_schema *schema){
    if(!schema||schema->version!=KSN_SCHEMA_ABI_VERSION||schema->slot_count>KSN_SCHEMA_MAX_SLOTS||
       schema->node_count>KSN_SCHEMA_MAX_NODES||
       (schema->slot_count&&!schema->slots)||(schema->node_count&&!schema->nodes))
        return KSN_INVALID;
    if(schema->dynamic_background&&
       (schema->background_slot>=schema->slot_count||
        schema->slots[schema->background_slot].type!=KSN_SLOT_COLOR))return KSN_INVALID;
    for(unsigned i=0;i<schema->slot_count;i++){
        const ksn_schema_slot *s=&schema->slots[i];
        if(!s->name||!s->name[0]||s->type>KSN_SLOT_U32||
           (s->type==KSN_SLOT_TEXT?(!s->capacity||s->capacity>KSN_SCHEMA_TEXT_MAX):
                                      s->capacity!=0)||
           (s->type==KSN_SLOT_U16?
                (s->initial_number>UINT16_MAX||s->maximum>UINT16_MAX||
                 (s->maximum&&s->initial_number>s->maximum)):
            s->type==KSN_SLOT_U32?
                (s->maximum&&s->initial_number>s->maximum):
                                      (s->initial_number||s->maximum)))return KSN_INVALID;
        for(unsigned j=0;j<i;j++)if(strcmp(s->name,schema->slots[j].name)==0)return KSN_INVALID;
    }
    for(unsigned i=0;i<schema->node_count;i++){
        const ksn_schema_node *n=&schema->nodes[i];
        if(n->kind>KSN_NODE_PLATE_TEXT||n->font>KSN_DISPLAY||
           (n->flags&~(KSN_SCHEMA_HAS_VISIBLE|KSN_SCHEMA_HAS_PAGE|KSN_SCHEMA_HAS_REVEAL))||
           !binding_valid(schema,&n->bounds,KSN_SLOT_RECT))return KSN_INVALID;
        if(n->rect_add_mask&~15u)return KSN_INVALID;
        for(unsigned edge=0;edge<4;edge++)if(n->rect_add_mask&(1u<<edge)){
            uint8_t slot=n->rect_add_slot[edge];
            if(slot>=schema->slot_count||schema->slots[slot].type!=KSN_SLOT_U16)
                return KSN_INVALID;
        }
        if(n->kind==KSN_NODE_PLATE_TEXT&&n->font!=KSN_CAPTION)return KSN_INVALID;
        if(n->kind==KSN_NODE_RECT&&n->radius)return KSN_INVALID;
        if(n->kind==KSN_NODE_ROUND_RECT&&n->radius>8u)return KSN_INVALID;
        if((n->flags&KSN_SCHEMA_HAS_VISIBLE)&&
           !binding_valid(schema,&n->visible,KSN_SLOT_BOOL))return KSN_INVALID;
        if((n->flags&KSN_SCHEMA_HAS_PAGE)&&
           !binding_valid(schema,&n->page,KSN_SLOT_U16))return KSN_INVALID;
        if((n->flags&KSN_SCHEMA_HAS_REVEAL)&&
           (n->kind!=KSN_NODE_TEXT||!binding_valid(schema,&n->reveal,KSN_SLOT_U16)))
            return KSN_INVALID;
        if(n->kind==KSN_NODE_IMAGE){
            if(!n->source_width||n->source_width>256||
               !n->source_height||n->source_height>256||
               !binding_valid(schema,&n->resource,KSN_SLOT_RESOURCE)||
               !binding_valid(schema,&n->variant,KSN_SLOT_U16)||
               !binding_valid(schema,&n->frame,KSN_SLOT_U16))return KSN_INVALID;
        }else{
            if(!binding_valid(schema,&n->color,KSN_SLOT_COLOR))return KSN_INVALID;
            if((n->kind==KSN_NODE_TEXT||n->kind==KSN_NODE_PLATE_TEXT)&&
               !binding_valid(schema,&n->text,KSN_SLOT_TEXT))return KSN_INVALID;
            if(n->kind==KSN_NODE_PLATE_TEXT&&
               !binding_valid(schema,&n->plate_color,KSN_SLOT_COLOR))return KSN_INVALID;
        }
    }
    return KSN_OK;
}

static ksn_result values_validate_checked(const ksn_schema *schema,
                                          const ksn_schema_value *values){
    if(schema->slot_count&&!values)return KSN_INVALID;
    for(unsigned i=0;i<schema->slot_count;i++){
        const ksn_schema_slot *slot=&schema->slots[i];
        if(slot->type==KSN_SLOT_TEXT&&
           !text_valid(values[i].data.text,slot->capacity))return KSN_INVALID;
        if(slot->type==KSN_SLOT_RECT){
            ksn_rect r=values[i].data.rect;
            if(r.x0>r.x1||r.y0>r.y1)return KSN_INVALID;
        }
        if(slot->type==KSN_SLOT_U16&&slot->maximum&&
           values[i].data.number>slot->maximum)return KSN_INVALID;
        if(slot->type==KSN_SLOT_U32&&slot->maximum&&
           values[i].data.wide_number>slot->maximum)return KSN_INVALID;
    }
    ksn_rgba background=schema->dynamic_background?
        values[schema->background_slot].data.color:schema->background;
    if((background&0xffu)!=0xffu)return KSN_INVALID;
    return KSN_OK;
}

ksn_result ksn_schema_values_validate(const ksn_schema *schema,
                                      const ksn_schema_value *values){
    return ksn_schema_validate(schema)==KSN_OK?
           values_validate_checked(schema,values):KSN_INVALID;
}

static void dependency_add(ksn_schema_dependencies *out,ksn_schema_binding binding,
                           unsigned node){
    if(binding.slot!=KSN_SCHEMA_LITERAL)out->nodes[binding.slot]|=1u<<node;
}
ksn_result ksn_schema_dependencies_build(const ksn_schema *schema,
                                         ksn_schema_dependencies *out){
    if(!out||ksn_schema_validate(schema)!=KSN_OK)return KSN_INVALID;
    *out=(ksn_schema_dependencies){0};
    if(schema->dynamic_background)
        out->background_slots=1u<<schema->background_slot;
    for(unsigned i=0;i<schema->node_count;i++){
        const ksn_schema_node *n=&schema->nodes[i];
        dependency_add(out,n->bounds,i);
        for(unsigned edge=0;edge<4;edge++)if(n->rect_add_mask&(1u<<edge))
            out->nodes[n->rect_add_slot[edge]]|=1u<<i;
        if(n->flags&KSN_SCHEMA_HAS_VISIBLE)dependency_add(out,n->visible,i);
        if(n->flags&KSN_SCHEMA_HAS_PAGE)dependency_add(out,n->page,i);
        if(n->kind==KSN_NODE_IMAGE){
            dependency_add(out,n->resource,i);
            dependency_add(out,n->variant,i);
            dependency_add(out,n->frame,i);
        }else{
            dependency_add(out,n->color,i);
            if(n->kind==KSN_NODE_TEXT||n->kind==KSN_NODE_PLATE_TEXT)
                dependency_add(out,n->text,i);
            if(n->kind==KSN_NODE_PLATE_TEXT)dependency_add(out,n->plate_color,i);
            if(n->flags&KSN_SCHEMA_HAS_REVEAL)dependency_add(out,n->reveal,i);
        }
    }
    return KSN_OK;
}

static ksn_rect get_rect(ksn_schema_binding b,const ksn_schema_value *v){
    return b.slot==KSN_SCHEMA_LITERAL?b.literal.rect:v[b.slot].data.rect;
}
static ksn_rgba get_color(ksn_schema_binding b,const ksn_schema_value *v){
    return b.slot==KSN_SCHEMA_LITERAL?b.literal.color:v[b.slot].data.color;
}
static ksn_schema_text get_text(ksn_schema_binding b,const ksn_schema_value *v){
    return b.slot==KSN_SCHEMA_LITERAL?b.literal.text:v[b.slot].data.text;
}
static uint16_t get_u16(ksn_schema_binding b,const ksn_schema_value *v){
    return b.slot==KSN_SCHEMA_LITERAL?b.literal.number:v[b.slot].data.number;
}
static bool get_bool(ksn_schema_binding b,const ksn_schema_value *v){
    return b.slot==KSN_SCHEMA_LITERAL?b.literal.boolean:v[b.slot].data.boolean;
}
static ksn_resource get_resource(ksn_schema_binding b,const ksn_schema_value *v){
    return b.slot==KSN_SCHEMA_LITERAL?b.literal.resource:v[b.slot].data.resource;
}
static ksn_rgba get_background(const ksn_schema *schema,const ksn_schema_value *values){
    return schema->dynamic_background?values[schema->background_slot].data.color:
                                      schema->background;
}
static bool offset_rect(ksn_rect *r,ksn_rect viewport){
    int32_t x0=(int32_t)r->x0+viewport.x0,x1=(int32_t)r->x1+viewport.x0;
    int32_t y0=(int32_t)r->y0+viewport.y0,y1=(int32_t)r->y1+viewport.y0;
    if(x0<INT16_MIN||x1>INT16_MAX||y0<INT16_MIN||y1>INT16_MAX)return false;
    *r=(ksn_rect){(int16_t)x0,(int16_t)y0,(int16_t)x1,(int16_t)y1};
    return true;
}
static int text_advance(ksn_schema_text text,ksn_font font){
    int width=0;
    for(unsigned i=0;i<text.bytes;){
        uint8_t lead=(uint8_t)text.utf8[i];
        unsigned n=lead<0x80?1:lead<0xe0?2:lead<0xf0?3:4;
        width+=(int)ksn_font_advance(font,lead);i+=n;
    }
    return width;
}
static ksn_result add(ksn_view *view,ksn_tx tx,ksn_draw *draw,
                      ksn_ref refs[KSN_SCHEMA_MAX_NODES*2u],uint8_t *count){
    if(*count==KSN_SCHEMA_MAX_NODES*2u)return KSN_LIMIT;
    ksn_result r=ksn_view_add(view,tx,draw,&refs[*count]);
    if(r==KSN_OK)(*count)++;
    return r;
}

typedef struct { uint8_t node;bool plate_tail; } schema_cursor;
typedef struct { ksn_draw draw;uint16_t reveal;bool has_reveal; } schema_resolved;
#ifdef KSN_SCHEMA_DIRTY_COUNT
uint32_t ksn_schema_resolved_nodes;
#endif
/* One node can expand to two commands. This cursor is the single execution
 * path for REPLACE, topology checks, and the changed-properties PATCH. */
static ksn_result next_draw_until(const ksn_schema *schema,
                                  const ksn_schema_value *values,
                                  ksn_rect viewport,schema_cursor *cursor,
                                  unsigned end_node,schema_resolved *out,
                                  bool *found){
    *found=false;
    while(cursor->node<end_node){
#ifdef KSN_SCHEMA_DIRTY_COUNT
        ksn_schema_resolved_nodes++;
#endif
        const ksn_schema_node *n=&schema->nodes[cursor->node];
        if(!cursor->plate_tail){
            if(((n->flags&KSN_SCHEMA_HAS_VISIBLE)&&!get_bool(n->visible,values))||
               ((n->flags&KSN_SCHEMA_HAS_PAGE)&&get_u16(n->page,values)!=n->page_equals)){
                cursor->node++;continue;
            }
        }
        ksn_schema_text value={0};
        if(n->kind==KSN_NODE_TEXT||n->kind==KSN_NODE_PLATE_TEXT){
            value=get_text(n->text,values);
            if(!value.bytes){cursor->node++;cursor->plate_tail=false;continue;}
        }
        ksn_rect bounds=get_rect(n->bounds,values);
        if(n->rect_add_mask){
            int32_t coords[4]={bounds.x0,bounds.y0,bounds.x1,bounds.y1};
            for(unsigned edge=0;edge<4;edge++)if(n->rect_add_mask&(1u<<edge)){
                coords[edge]+=values[n->rect_add_slot[edge]].data.number;
                if(coords[edge]>INT16_MAX)return KSN_INVALID;
            }
            bounds=(ksn_rect){(int16_t)coords[0],(int16_t)coords[1],
                              (int16_t)coords[2],(int16_t)coords[3]};
        }
        if(bounds.x0>bounds.x1||bounds.y0>bounds.y1)return KSN_INVALID;
        if(bounds.x0==bounds.x1||bounds.y0==bounds.y1){
            cursor->node++;cursor->plate_tail=false;continue;
        }
        if(!offset_rect(&bounds,viewport))return KSN_INVALID;
        if(n->kind==KSN_NODE_ROUND_RECT&&
           ((unsigned)n->radius>(unsigned)(bounds.x1-bounds.x0)/2u||
            (unsigned)n->radius>(unsigned)(bounds.y1-bounds.y0)/2u))return KSN_INVALID;
        *out=(schema_resolved){.draw={.opacity=255,.bounds=bounds,.clip=viewport}};
        ksn_draw *draw=&out->draw;
        if(n->kind==KSN_NODE_PLATE_TEXT){
            int width=text_advance(value,n->font);
            if((int32_t)bounds.x0+width+8>INT16_MAX||
               (int32_t)bounds.y0+16>INT16_MAX)return KSN_INVALID;
            if(!cursor->plate_tail){
                draw->kind=KSN_RECT;
                draw->bounds=(ksn_rect){bounds.x0,bounds.y0,
                                         (int16_t)(bounds.x0+width+8),
                                         (int16_t)(bounds.y0+16)};
                draw->data.shape.color=get_color(n->plate_color,values);
                cursor->plate_tail=true;*found=true;return KSN_OK;
            }
            draw->bounds=(ksn_rect){(int16_t)(bounds.x0+4),(int16_t)(bounds.y0+2),
                                     (int16_t)(bounds.x0+4+width),(int16_t)(bounds.y0+14)};
            cursor->plate_tail=false;
        }
        cursor->node++;
        if(n->kind==KSN_NODE_RECT||n->kind==KSN_NODE_ROUND_RECT){
            draw->kind=n->kind==KSN_NODE_RECT?KSN_RECT:KSN_ROUND_RECT;
            draw->data.shape.color=get_color(n->color,values);
            draw->data.shape.radius=n->radius;
        }else if(n->kind==KSN_NODE_TEXT||n->kind==KSN_NODE_PLATE_TEXT){
            draw->kind=KSN_TEXT;draw->data.text.utf8=value.utf8;
            draw->data.text.bytes=value.bytes;
            draw->data.text.capacity=n->text.slot==KSN_SCHEMA_LITERAL?
                                      value.bytes:schema->slots[n->text.slot].capacity;
            draw->data.text.font=n->font;
            draw->data.text.color=get_color(n->color,values);
            out->has_reveal=(n->flags&KSN_SCHEMA_HAS_REVEAL)!=0;
            if(out->has_reveal)out->reveal=get_u16(n->reveal,values);
            else for(unsigned b=0;b<value.bytes;b++)
                if(((uint8_t)value.utf8[b]&0xc0u)!=0x80u)out->reveal++;
            if(out->has_reveal){
                unsigned scalars=0;
                for(unsigned b=0;b<value.bytes;b++)
                    if(((uint8_t)value.utf8[b]&0xc0u)!=0x80u)scalars++;
                if(out->reveal>scalars)return KSN_INVALID;
            }
        }else{
            draw->kind=KSN_IMAGE;
            draw->data.image.resource=get_resource(n->resource,values);
            draw->data.image.variant=get_u16(n->variant,values);
            draw->data.image.frame=get_u16(n->frame,values);
            draw->data.image.source_width=n->source_width;
            draw->data.image.source_height=n->source_height;
            draw->data.image.scale=KSN_IMAGE_STRETCH;
            if(!draw->data.image.resource.value||draw->data.image.variant>255||
               draw->data.image.frame>255)return KSN_INVALID;
        }
        *found=true;return KSN_OK;
    }
    return KSN_OK;
}
static ksn_result next_draw(const ksn_schema *schema,const ksn_schema_value *values,
                            ksn_rect viewport,schema_cursor *cursor,
                            schema_resolved *out,bool *found){
    return next_draw_until(schema,values,viewport,cursor,schema->node_count,
                           out,found);
}

ksn_result ksn_schema_ref_map_build(const ksn_schema *schema,
                                    const ksn_schema_value *values,
                                    ksn_rect viewport,ksn_schema_ref_map *out){
    if(!out||viewport.x0>=viewport.x1||viewport.y0>=viewport.y1||
       ksn_schema_values_validate(schema,values)!=KSN_OK)return KSN_INVALID;
    ksn_schema_ref_map map={0};
    unsigned total=0;
    for(unsigned node=0;node<schema->node_count;node++){
        schema_cursor cursor={(uint8_t)node,false};
        for(;;){
            schema_resolved resolved;bool found;
            ksn_result r=next_draw_until(schema,values,viewport,&cursor,node+1u,
                                         &resolved,&found);
            if(r!=KSN_OK)return r;
            if(!found)break;
            if(total==KSN_SCHEMA_MAX_NODES*2u)return KSN_LIMIT;
            if(!(map.present&((uint32_t)1u<<node)))
                map.present|=(uint32_t)1u<<node;
            else if(!(map.extra&((uint32_t)1u<<node)))
                map.extra|=(uint32_t)1u<<node;
            else return KSN_LIMIT;
            total++;
        }
    }
    *out=map;return KSN_OK;
}

static ksn_result preflight(const ksn_view *view,const ksn_schema *schema,
                            const ksn_schema_value *values,ksn_rect viewport){
    if(viewport.x0>=viewport.x1||viewport.y0>=viewport.y1||
       ksn_schema_values_validate(schema,values)!=KSN_OK)return KSN_INVALID;
    schema_cursor cursor={0};unsigned commands=0,text_bytes=0;bool found;
    for(;;){
        schema_resolved resolved;
        ksn_result r=next_draw(schema,values,viewport,&cursor,&resolved,&found);
        if(r!=KSN_OK)return r;
        if(!found)break;
        if(view){
            r=ksn_view_check_draw(view,&resolved.draw);
            if(r!=KSN_OK)return r;
        }
        commands++;
        if(resolved.draw.kind==KSN_TEXT)text_bytes+=resolved.draw.data.text.capacity;
        if(commands>KSN_APP_COMMANDS||text_bytes>KSN_APP_TEXT_BYTES)return KSN_LIMIT;
    }
    return KSN_OK;
}
ksn_result ksn_schema_preflight(const ksn_schema *schema,
                                const ksn_schema_value *values,ksn_rect viewport){
    return preflight(NULL,schema,values,viewport);
}
ksn_result ksn_schema_preflight_view(const ksn_view *view,const ksn_schema *schema,
                                     const ksn_schema_value *values,ksn_rect viewport){
    return view?preflight(view,schema,values,viewport):KSN_INVALID;
}

ksn_result ksn_schema_submit(ksn_view *view,ksn_rect viewport,
                             const ksn_schema *schema,
                             const ksn_schema_value *values,
                             ksn_ref refs[KSN_SCHEMA_MAX_NODES*2u],
                             uint8_t *ref_count,ksn_tx *out){
    if(!view||!refs||!ref_count||!out||viewport.x0>=viewport.x1||
       viewport.y0>=viewport.y1||ksn_schema_values_validate(schema,values)!=KSN_OK)
        return KSN_INVALID;
    ksn_tx tx;ksn_result r=ksn_view_begin(view,KSN_REPLACE,&tx);
    if(r!=KSN_OK)return r;
    r=ksn_view_background(view,tx,get_background(schema,values));
    uint8_t count=0;
    schema_cursor cursor={0};bool found;
    while(r==KSN_OK){
        schema_resolved resolved;
        r=next_draw(schema,values,viewport,&cursor,&resolved,&found);
        if(r!=KSN_OK||!found)break;
        r=add(view,tx,&resolved.draw,refs,&count);
        if(r==KSN_OK&&resolved.has_reveal){
            ksn_change change={.property=KSN_SET_REVEAL,
                               .value.reveal=resolved.reveal};
            r=ksn_view_change(view,tx,refs[count-1u],&change);
        }
    }
    if(r==KSN_OK)r=ksn_view_submit(view,tx);
    if(r!=KSN_OK){(void)ksn_view_cancel(view,tx);return r;}
    *ref_count=count;*out=tx;return KSN_OK;
}

static bool rect_equal(ksn_rect a,ksn_rect b){
    return a.x0==b.x0&&a.y0==b.y0&&a.x1==b.x1&&a.y1==b.y1;
}
static bool same_topology(const schema_resolved *want,const ksn_view_snapshot *have){
    const ksn_draw *a=&want->draw,*b=&have->draw;
    if(!have->visible||a->kind!=b->kind||a->opacity!=b->opacity||
       !rect_equal(a->clip,b->clip))return false;
    if(a->kind==KSN_ROUND_RECT&&a->data.shape.radius!=b->data.shape.radius)
        return false;
    if(a->kind==KSN_TEXT&&
       (a->data.text.capacity!=b->data.text.capacity||
        a->data.text.font!=b->data.text.font))return false;
    if(a->kind==KSN_IMAGE&&
       (a->data.image.resource.value!=b->data.image.resource.value||
        a->data.image.scale!=b->data.image.scale||
        a->data.image.source_width!=b->data.image.source_width||
        a->data.image.source_height!=b->data.image.source_height||
        a->data.image.source_x!=b->data.image.source_x||
        a->data.image.source_y!=b->data.image.source_y||
        a->data.image.rotation!=b->data.image.rotation))return false;
    return true;
}
static bool changed(const schema_resolved *want,const ksn_view_snapshot *have){
    const ksn_draw *a=&want->draw,*b=&have->draw;
    if(!rect_equal(a->bounds,b->bounds))return true;
    if(a->kind==KSN_IMAGE)return a->data.image.variant!=b->data.image.variant||
                                  a->data.image.frame!=b->data.image.frame;
    if(a->kind==KSN_TEXT)return a->data.text.color!=b->data.text.color||
        a->data.text.bytes!=b->data.text.bytes||
        memcmp(a->data.text.utf8,b->data.text.utf8,a->data.text.bytes)!=0||
        want->reveal!=have->reveal;
    return a->data.shape.color!=b->data.shape.color;
}
static ksn_result patch_one(ksn_view *view,ksn_tx tx,ksn_ref ref,
                            const schema_resolved *want,const ksn_view_snapshot *have){
    const ksn_draw *a=&want->draw,*b=&have->draw;
    ksn_result r=KSN_OK;
    if(!rect_equal(a->bounds,b->bounds)){
        ksn_change c={.property=KSN_SET_RECT,.value.rect=a->bounds};
        r=ksn_view_change(view,tx,ref,&c);
    }
    if(r!=KSN_OK)return r;
    if(a->kind==KSN_IMAGE){
        if(a->data.image.variant!=b->data.image.variant||
           a->data.image.frame!=b->data.image.frame){
            ksn_change c={.property=KSN_SET_IMAGE_FRAME,
                          .value.image={a->data.image.variant,a->data.image.frame}};
            r=ksn_view_change(view,tx,ref,&c);
        }
        return r;
    }
    ksn_rgba ac=a->kind==KSN_TEXT?a->data.text.color:a->data.shape.color;
    ksn_rgba bc=a->kind==KSN_TEXT?b->data.text.color:b->data.shape.color;
    if(ac!=bc){
        ksn_change c={.property=KSN_SET_COLOR,.value.color=ac};
        r=ksn_view_change(view,tx,ref,&c);
    }
    if(r!=KSN_OK||a->kind!=KSN_TEXT)return r;
    bool text_diff=a->data.text.bytes!=b->data.text.bytes||
                   memcmp(a->data.text.utf8,b->data.text.utf8,a->data.text.bytes)!=0;
    if(text_diff){
        ksn_change c={.property=KSN_SET_TEXT,
                      .value.text={a->data.text.utf8,a->data.text.bytes}};
        r=ksn_view_change(view,tx,ref,&c);
    }
    if(r==KSN_OK&&(want->reveal!=have->reveal||
                   (text_diff&&want->has_reveal))){
        ksn_change c={.property=KSN_SET_REVEAL,.value.reveal=want->reveal};
        r=ksn_view_change(view,tx,ref,&c);
    }
    return r;
}

ksn_result ksn_schema_update(ksn_view *view,ksn_rect viewport,
                             const ksn_schema *schema,
                             const ksn_schema_value *values,
                             const ksn_ref active_refs[KSN_SCHEMA_MAX_NODES*2u],
                             uint8_t active_count,ksn_rgba active_background,
                             ksn_ref candidate_refs[KSN_SCHEMA_MAX_NODES*2u],
                             uint8_t *candidate_count,ksn_tx *out,
                             ksn_schema_delta *delta){
    if(!view||!candidate_refs||!candidate_count||!out||!delta||
       viewport.x0>=viewport.x1||viewport.y0>=viewport.y1||
       (active_count!=UINT8_MAX&&active_count&&!active_refs)||
       !schema||values_validate_checked(schema,values)!=KSN_OK)return KSN_INVALID;
    bool replace=active_count==UINT8_MAX||
                 get_background(schema,values)!=active_background;
    bool any_change=false;
    uint8_t count=0;schema_cursor cursor={0};bool found;
    while(!replace){
        schema_resolved resolved;ksn_result r=next_draw(schema,values,viewport,&cursor,
                                                        &resolved,&found);
        if(r!=KSN_OK)return r;
        if(!found)break;
        if(count>=active_count){replace=true;break;}
        ksn_view_snapshot old;
        r=ksn_view_read_ref(view,active_refs[count],&old);
        if(r!=KSN_OK||!same_topology(&resolved,&old)){replace=true;break;}
        if(changed(&resolved,&old))any_change=true;
        count++;
    }
    if(!replace&&count!=active_count)replace=true;
    if(replace){
        ksn_result r=ksn_schema_submit(view,viewport,schema,values,candidate_refs,
                                       candidate_count,out);
        if(r==KSN_OK)*delta=KSN_SCHEMA_REPLACED;
        return r;
    }
    *candidate_count=active_count;
    if(!any_change){*out=(ksn_tx){0};*delta=KSN_SCHEMA_NO_CHANGE;return KSN_OK;}
    ksn_tx tx;ksn_result r=ksn_view_begin(view,KSN_PATCH,&tx);
    if(r!=KSN_OK)return r;
    cursor=(schema_cursor){0};count=0;
    while(r==KSN_OK){
        schema_resolved resolved;
        r=next_draw(schema,values,viewport,&cursor,&resolved,&found);
        if(r!=KSN_OK||!found)break;
        ksn_view_snapshot old;
        r=ksn_view_read_ref(view,active_refs[count],&old);
        if(r==KSN_OK&&changed(&resolved,&old))
            r=patch_one(view,tx,active_refs[count],&resolved,&old);
        count++;
    }
    if(r==KSN_OK)r=ksn_view_submit(view,tx);
    if(r!=KSN_OK){(void)ksn_view_cancel(view,tx);return r;}
    *out=tx;*delta=KSN_SCHEMA_PATCHED;return KSN_OK;
}

static ksn_result replace_mapped(ksn_view *view,ksn_rect viewport,
    const ksn_schema *schema,const ksn_schema_value *values,
    ksn_ref candidate_refs[KSN_SCHEMA_MAX_NODES*2u],
    ksn_schema_ref_map *candidate_map,uint8_t *candidate_count,ksn_tx *out,
    ksn_schema_delta *delta){
    ksn_schema_ref_map map;
    ksn_result r=ksn_schema_ref_map_build(schema,values,viewport,&map);
    if(r!=KSN_OK)return r;
    r=ksn_schema_submit(view,viewport,schema,values,candidate_refs,
                        candidate_count,out);
    if(r!=KSN_OK)return r;
    if(*candidate_count!=ksn_schema_ref_map_total(&map)){
        (void)ksn_view_cancel(view,*out);
        return KSN_STALE;
    }
    *candidate_map=map;*delta=KSN_SCHEMA_REPLACED;
    return KSN_OK;
}

ksn_result ksn_schema_update_dirty(ksn_view *view,ksn_rect viewport,
    const ksn_schema *schema,const ksn_schema_value *values,uint32_t dirty_nodes,
    const ksn_ref active_refs[KSN_SCHEMA_MAX_NODES*2u],uint8_t active_count,
    const ksn_schema_ref_map *active_map,ksn_rgba active_background,
    ksn_ref candidate_refs[KSN_SCHEMA_MAX_NODES*2u],
    ksn_schema_ref_map *candidate_map,uint8_t *candidate_count,ksn_tx *out,
    ksn_schema_delta *delta){
    if(!view||!schema||!candidate_refs||!candidate_map||!candidate_count||
       !out||!delta||viewport.x0>=viewport.x1||viewport.y0>=viewport.y1||
       values_validate_checked(schema,values)!=KSN_OK||
       (schema->node_count<32u&&(dirty_nodes>>schema->node_count)))
        return KSN_INVALID;
    bool replace=active_count==UINT8_MAX||
                 get_background(schema,values)!=active_background;
    if(!replace&&(!active_refs||!active_map||
                  ksn_schema_ref_map_total(active_map)!=active_count))
        return KSN_INVALID;
    if(replace)return replace_mapped(view,viewport,schema,values,candidate_refs,
                                    candidate_map,candidate_count,out,delta);
    bool any_change=false;
    for(unsigned node=0;node<schema->node_count;node++){
        if(!(dirty_nodes&((uint32_t)1u<<node)))continue;
        unsigned index=ksn_schema_ref_map_start(active_map,node);
        unsigned end=index+ksn_schema_ref_map_count(active_map,node);
        if(end>active_count){replace=true;break;}
        schema_cursor cursor={(uint8_t)node,false};
        for(;;){
            schema_resolved resolved;bool found;
            ksn_result r=next_draw_until(schema,values,viewport,&cursor,node+1u,
                                         &resolved,&found);
            if(r!=KSN_OK)return r;
            if(!found)break;
            if(index>=end){replace=true;break;}
            ksn_view_snapshot old;
            r=ksn_view_read_ref(view,active_refs[index],&old);
            if(r!=KSN_OK||!same_topology(&resolved,&old)){replace=true;break;}
            if(changed(&resolved,&old))any_change=true;
            index++;
        }
        if(replace||index!=end){replace=true;break;}
    }
    if(replace)return replace_mapped(view,viewport,schema,values,candidate_refs,
                                    candidate_map,candidate_count,out,delta);
    *candidate_count=active_count;
    *candidate_map=*active_map;
    if(!any_change){*out=(ksn_tx){0};*delta=KSN_SCHEMA_NO_CHANGE;return KSN_OK;}
    ksn_tx tx;ksn_result r=ksn_view_begin(view,KSN_PATCH,&tx);
    if(r!=KSN_OK)return r;
    for(unsigned node=0;node<schema->node_count&&r==KSN_OK;node++){
        if(!(dirty_nodes&((uint32_t)1u<<node)))continue;
        unsigned index=ksn_schema_ref_map_start(active_map,node);
        schema_cursor cursor={(uint8_t)node,false};
        for(;;){
            schema_resolved resolved;bool found;
            r=next_draw_until(schema,values,viewport,&cursor,node+1u,
                              &resolved,&found);
            if(r!=KSN_OK||!found)break;
            ksn_view_snapshot old;
            r=ksn_view_read_ref(view,active_refs[index],&old);
            if(r==KSN_OK&&changed(&resolved,&old))
                r=patch_one(view,tx,active_refs[index],&resolved,&old);
            index++;
        }
    }
    if(r==KSN_OK)r=ksn_view_submit(view,tx);
    if(r!=KSN_OK){(void)ksn_view_cancel(view,tx);return r;}
    *out=tx;*delta=KSN_SCHEMA_PATCHED;return KSN_OK;
}
