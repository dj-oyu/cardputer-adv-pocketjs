#include "core_fixture.h"
#include "ksn_schema_session.h"
#include "ksn_view_host.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do {if(!(x)){fprintf(stderr,"schema workloads line %d: %s\n",__LINE__,#x);return 1;}}while(0)
#define LIT KSN_SCHEMA_LITERAL
KSN_TEST_CORE(fast_core,static);
KSN_TEST_CORE(ref_core,static);
static ksn_cache fast_cache,ref_cache;
static ksn_cache_command_block fast_cache_commands,ref_cache_commands;
static ksn_cache_text_block fast_cache_text,ref_cache_text;
typedef struct {uint16_t strip[240*8],panel[240*135];} display;
static display fast_display,ref_display;
static uint16_t *strip(void *ctx){return ((display *)ctx)->strip;}
static ksn_result span(void *ctx,const ksn_draw *draw,uint16_t reveal,
                       int x,int y,unsigned count,uint8_t *out){
    (void)ctx;(void)draw;(void)reveal;
    for(unsigned i=0;i<count;i++)out[i]=((x+(int)i+y)&1)?255:128;
    return KSN_OK;
}
static const ksn_text_port text_port={.span=span};
static ksn_result transfer(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    memcpy(((display *)ctx)->panel+y*240,pixels,(size_t)rows*240*sizeof(*pixels));
    return KSN_OK;
}
static ksn_display_port port(display *d){
    return (ksn_display_port){d,strip,transfer,240,135,8,&text_port,NULL};
}
static int compare_at(const ksn_schema *schema,const ksn_schema_value *values,
                      ksn_rect viewport,ksn_view_host *reference,
                      ksn_view *ref_view){
    ksn_ref refs[KSN_SCHEMA_MAX_NODES*2u];uint8_t count;ksn_tx tx;
    ksn_render_stats stats;
    if(ksn_schema_submit(ref_view,viewport,schema,values,refs,&count,&tx)!=KSN_OK)return 1;
    ksn_display_port out=port(&ref_display);
    if(ksn_view_host_present(reference,&out,&stats)!=KSN_OK)return 1;
    return memcmp(fast_display.panel,ref_display.panel,sizeof(ref_display.panel))!=0;
}
static int compare(const ksn_schema *schema,const ksn_schema_value *values,
                   ksn_view_host *reference,ksn_view *ref_view){
    return compare_at(schema,values,(ksn_rect){0,0,240,135},reference,ref_view);
}
static int present(ksn_schema_session *session,ksn_view_host *host,ksn_view *view,
                   const ksn_schema_value *values,uint64_t revision,
                   uint32_t dirty_slots){
    ksn_rect viewport={0,0,240,135};ksn_render_stats stats;bool blocked=false;
    ksn_display_port out=port(&fast_display);
    ksn_result r=ksn_schema_session_step_dirty(session,view,viewport,values,
                                               revision,dirty_slots,&blocked);
    if(r!=KSN_OK||!blocked){fprintf(stderr,"workload submit revision=%llu result=%u blocked=%u\n",
        (unsigned long long)revision,(unsigned)r,(unsigned)blocked);return 1;}
    r=ksn_view_host_present(host,&out,&stats);
    if(r!=KSN_OK){fprintf(stderr,"workload present revision=%llu result=%u\n",
        (unsigned long long)revision,(unsigned)r);return 1;}
    r=ksn_schema_session_step_dirty(session,view,viewport,values,revision,0,&blocked);
    if(r!=KSN_OK||blocked){fprintf(stderr,"workload ack revision=%llu result=%u blocked=%u\n",
        (unsigned long long)revision,(unsigned)r,(unsigned)blocked);return 1;}
    return 0;
}
int main(void){
    static char names[24][8];
    static ksn_schema_slot slots[24];
    static ksn_schema_node nodes[23];
    ksn_schema_value values[24]={0};
    for(unsigned i=0;i<24;i++){
        (void)snprintf(names[i],sizeof(names[i]),"s%u",i);
        slots[i].name=names[i];
    }
    slots[0].type=KSN_SLOT_TEXT;slots[0].capacity=47;
    values[0].data.text=(ksn_schema_text){"START",5};
    nodes[0]=(ksn_schema_node){.kind=KSN_NODE_TEXT,
        .bounds={.slot=LIT,.literal.rect={4,4,236,18}},
        .color={.slot=LIT,.literal.color=0xf0f8ffffu},
        .text={.slot=0},.font=KSN_CAPTION};
    for(unsigned i=0;i<20;i++){
        slots[i+1].type=KSN_SLOT_COLOR;
        values[i+1].data.color=0x507090ffu+(i<<24);
        int x=8+(int)(i%10)*22,y=30+(int)(i/10)*22;
        nodes[i+1]=(ksn_schema_node){.kind=KSN_NODE_RECT,
            .bounds={.slot=LIT,.literal.rect={(int16_t)x,(int16_t)y,
                                             (int16_t)(x+16),(int16_t)(y+16)}},
            .color={.slot=(uint8_t)(i+1)}};
    }
    slots[21].type=KSN_SLOT_BOOL;values[21].data.boolean=false;
    nodes[21]=(ksn_schema_node){.kind=KSN_NODE_RECT,
        .bounds={.slot=LIT,.literal.rect={8,85,55,95}},
        .color={.slot=LIT,.literal.color=0xe0b070ffu},
        .flags=KSN_SCHEMA_HAS_VISIBLE,.visible={.slot=21}};
    slots[22].type=KSN_SLOT_U16;values[22].data.number=0;
    slots[23].type=KSN_SLOT_TEXT;slots[23].capacity=47;
    values[23].data.text=(ksn_schema_text){"TAG",3};
    nodes[22]=(ksn_schema_node){.kind=KSN_NODE_PLATE_TEXT,
        .bounds={.slot=LIT,.literal.rect={8,103,9,119}},
        .color={.slot=LIT,.literal.color=0xffffffffu},
        .plate_color={.slot=LIT,.literal.color=0x102030ffu},
        .text={.slot=23},.font=KSN_CAPTION,
        .flags=KSN_SCHEMA_HAS_PAGE,.page={.slot=22},.page_equals=1};
    ksn_schema schema={.version=KSN_SCHEMA_ABI_VERSION,.slot_count=24,.node_count=23,
        .background=0x071425ffu,.slots=slots,.nodes=nodes};
    CHECK(ksn_schema_validate(&schema)==KSN_OK);
    CHECK(ksn_cache_bind(&fast_cache,&fast_cache_commands,&fast_cache_text)==KSN_OK);
    CHECK(ksn_cache_bind(&ref_cache,&ref_cache_commands,&ref_cache_text)==KSN_OK);
    ksn_view_host fast_host,ref_host;
    ksn_view_host_init(&fast_host,&fast_core,&fast_cache,17);
    ksn_view_host_init(&ref_host,&ref_core,&ref_cache,17);
    ksn_view *fast_view=ksn_view_host_endpoint(&fast_host,KSN_APP);
    ksn_view *ref_view=ksn_view_host_endpoint(&ref_host,KSN_APP);
    ksn_schema_session session;
    CHECK(ksn_schema_session_init(&session,&schema)==KSN_OK);
    CHECK(!present(&session,&fast_host,fast_view,values,1,0));
    CHECK(session.has_map&&ksn_schema_ref_map_total(&session.active_map)==21);
    CHECK(!compare(&schema,values,&ref_host,ref_view));
    bool blocked=true;
    CHECK(ksn_schema_session_step_dirty(&session,fast_view,(ksn_rect){0,0,240,135},
                                        values,1,0,&blocked)==KSN_OK&&!blocked);
    values[1].data.color=0xd06040ffu;
#ifdef KSN_SCHEMA_DIRTY_COUNT
    ksn_schema_resolved_nodes=0;
#endif
    CHECK(!present(&session,&fast_host,fast_view,values,2,1u<<1));
#ifdef KSN_SCHEMA_DIRTY_COUNT
    CHECK(ksn_schema_resolved_nodes==2);
#endif
    CHECK(session.pending_delta==KSN_SCHEMA_PATCHED);
    CHECK(!compare(&schema,values,&ref_host,ref_view));
    static const char max_text[]="12345678901234567890123456789012345678901234567";
    _Static_assert(sizeof(max_text)-1==47,"maximum text fixture");
    values[0].data.text=(ksn_schema_text){max_text,47};
    for(unsigned i=1;i<=20;i++)values[i].data.color=0x203040ffu+(i<<16);
    values[21].data.boolean=true;values[22].data.number=1;
    values[23].data.text=(ksn_schema_text){"PAGE",4};
    CHECK(!present(&session,&fast_host,fast_view,values,3,0xffffffu));
    CHECK(session.pending_delta==KSN_SCHEMA_REPLACED);
    CHECK(ksn_schema_ref_map_count(&session.active_map,22)==2);
    CHECK(!compare(&schema,values,&ref_host,ref_view));
    values[2].data.color=0x112233ffu;
    CHECK(ksn_schema_session_step_dirty(&session,fast_view,(ksn_rect){0,0,240,135},
                                        values,4,1u<<2,&blocked)==KSN_OK&&blocked);
    values[2].data.color=0x445566ffu;
    CHECK(ksn_schema_session_step_dirty(&session,fast_view,(ksn_rect){0,0,240,135},
                                        values,5,1u<<2,&blocked)==KSN_OK&&blocked);
    ksn_display_port out=port(&fast_display);ksn_render_stats stats;
    CHECK(ksn_view_host_present(&fast_host,&out,&stats)==KSN_OK);
    CHECK(ksn_schema_session_step_dirty(&session,fast_view,(ksn_rect){0,0,240,135},
                                        values,5,0,&blocked)==KSN_OK&&blocked);
    CHECK(ksn_view_host_present(&fast_host,&out,&stats)==KSN_OK);
    CHECK(ksn_schema_session_step_dirty(&session,fast_view,(ksn_rect){0,0,240,135},
                                        values,5,0,&blocked)==KSN_OK&&!blocked);
    CHECK(!compare(&schema,values,&ref_host,ref_view));
    values[21].data.boolean=false;values[22].data.number=0;
    CHECK(ksn_schema_session_step_dirty(&session,fast_view,(ksn_rect){0,0,240,135},
                                        values,6,(1u<<21)|(1u<<22),&blocked)==KSN_OK&&blocked);
    CHECK(ksn_view_cancel(fast_view,session.ticket)==KSN_OK);
    CHECK(!present(&session,&fast_host,fast_view,values,6,0));
    CHECK(!compare(&schema,values,&ref_host,ref_view));
    ksn_rect narrower={0,0,239,135};
    CHECK(ksn_schema_session_step_dirty(&session,fast_view,narrower,values,7,0,
                                        &blocked)==KSN_OK&&blocked);
    CHECK(session.pending_delta==KSN_SCHEMA_REPLACED);
    CHECK(ksn_view_host_present(&fast_host,&out,&stats)==KSN_OK);
    CHECK(ksn_schema_session_step_dirty(&session,fast_view,narrower,values,7,0,
                                        &blocked)==KSN_OK&&!blocked);
    CHECK(!compare_at(&schema,values,narrower,&ref_host,ref_view));
    CHECK(ksn_schema_session_step_dirty(&session,fast_view,
        (ksn_rect){0,0,240,135},values,8,0,&blocked)==KSN_OK&&blocked);
    CHECK(session.pending_delta==KSN_SCHEMA_REPLACED);
    CHECK(ksn_view_host_present(&fast_host,&out,&stats)==KSN_OK);
    CHECK(ksn_schema_session_step_dirty(&session,fast_view,
        (ksn_rect){0,0,240,135},values,8,0,&blocked)==KSN_OK&&!blocked);
    CHECK(!compare(&schema,values,&ref_host,ref_view));
    uint32_t seed=0x63a24e91u;
    for(unsigned frame=0;frame<120;frame++){
        seed=seed*1664525u+1013904223u;
        unsigned slot=1u+seed%20u;
        ksn_rgba next=(seed&0xffffff00u)|0xffu;
        if(next==values[slot].data.color)next^=0x01000000u;
        values[slot].data.color=next;
#ifdef KSN_SCHEMA_DIRTY_COUNT
        ksn_schema_resolved_nodes=0;
#endif
        CHECK(!present(&session,&fast_host,fast_view,values,9u+frame,
                       (uint32_t)1u<<slot));
        CHECK(session.pending_delta==KSN_SCHEMA_PATCHED);
#ifdef KSN_SCHEMA_DIRTY_COUNT
        CHECK(ksn_schema_resolved_nodes==2);
#endif
        CHECK(!compare(&schema,values,&ref_host,ref_view));
    }
    /* Mixed deterministic fuzz: every turn changes a visible color, while
     * text length, command count, visibility and page vary independently.
     * The full-scan renderer is rebuilt every turn and compared pixelwise. */
    for(unsigned frame=0;frame<600;frame++){
        seed=seed*1664525u+1013904223u;
        unsigned slot=1u+seed%20u;
        values[slot].data.color=((seed^frame)&0xffffff00u)|0xffu;
        uint32_t dirty=1u<<slot;
        switch(frame%6u){
        case 0:
            values[21].data.boolean=!values[21].data.boolean;
            dirty|=1u<<21;
            break;
        case 1:
            values[22].data.number^=1u;
            dirty|=1u<<22;
            break;
        case 2:
            values[0].data.text=(frame&2u)?
                (ksn_schema_text){max_text,47}:(ksn_schema_text){"",0};
            dirty|=1u;
            break;
        case 3:
            values[23].data.text=(frame&2u)?
                (ksn_schema_text){"LONG LABEL",10}:(ksn_schema_text){"",0};
            dirty|=1u<<23;
            break;
        case 4:
            for(unsigned i=1;i<=20;i+=3){
                values[i].data.color=((seed+i*0x102030u)&0xffffff00u)|0xffu;
                dirty|=1u<<i;
            }
            break;
        default:
            for(unsigned i=1;i<=20;i++)
                values[i].data.color=((seed+i*0x10305u)&0xffffff00u)|0xffu;
            values[0].data.text=(ksn_schema_text){max_text,47};
            values[21].data.boolean=!values[21].data.boolean;
            values[22].data.number^=1u;
            values[23].data.text=(ksn_schema_text){"PAGE",4};
            dirty=0xffffffu;
            break;
        }
        CHECK(!present(&session,&fast_host,fast_view,values,129u+frame,dirty));
        CHECK(!compare(&schema,values,&ref_host,ref_view));
    }
    puts("schema workloads: PASS (dirty node, 1/24 slots, 47-byte text, hidden/page, coalesced, discard, viewport, 120 single-slot + 600 mixed reference frames)");
    return 0;
}
