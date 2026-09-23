#include "app_view_assets.h"
#include "ui/kasane/ksn_p0_probe.h"
#include "system/sys_device.h"
#include "pet/ksn_pet.h"
#include <stdio.h>
#include <string.h>

#define LIT KSN_SCHEMA_LITERAL
#define RECT(a,b,c,d) {.slot=LIT,.literal.rect={a,b,c,d}}
#define COLOR(c) {.slot=LIT,.literal.color=c}
#define TEXT(s) {.slot=LIT,.literal.text={s,(uint16_t)(sizeof(s)-1u)}}
static const ksn_schema_slot hello_slots[]={
    {"counter",KSN_SLOT_TEXT,KSN_SCHEMA_TEXT_MAX,0,0}
};
static const ksn_schema_node hello_nodes[]={
    {.kind=KSN_NODE_TEXT,.bounds=RECT(16,12,226,22),.color=COLOR(0x69cdeeffu),
     .text=TEXT("KASANE / JAVASCRIPT"),.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(16,34,236,52),.color=COLOR(0xf0f8ffffu),
     .text=TEXT("Hello, World!"),.font=KSN_DISPLAY},
    {.kind=KSN_NODE_ROUND_RECT,.bounds=RECT(16,62,224,100),
     .color=COLOR(0x12334affu),.radius=6},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(28,77,212,89),.color=COLOR(0x8ef0c4ffu),
     .text={.slot=0},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(16,117,236,127),.color=COLOR(0xa9bacaffu),
     .text=TEXT("ENTER +1    ESC HOME"),.font=KSN_CAPTION}
};
static const ksn_schema hello={.version=1,.slot_count=1,.node_count=5,
    .background=0x071425ffu,.slots=hello_slots,.nodes=hello_nodes};

static const ksn_schema_slot imucal_slots[]={
    {"head",KSN_SLOT_TEXT,47,0,0},{"live",KSN_SLOT_TEXT,47,0,0},
    {"stat",KSN_SLOT_TEXT,47,0,0},{"spin",KSN_SLOT_TEXT,47,0,0},
    {"foot",KSN_SLOT_TEXT,47,0,0}
};
static const ksn_schema_node imucal_nodes[]={
    {.kind=KSN_NODE_TEXT,.bounds=RECT(12,8,228,20),.color=COLOR(0x69cdeeffu),
     .text=TEXT("IMU AXIS CALIBRATION"),.font=KSN_BODY},
    {.kind=KSN_NODE_ROUND_RECT,.bounds=RECT(12,48,228,94),
     .color=COLOR(0x12334affu),.radius=5},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(12,26,228,38),.color=COLOR(0xf0f8ffffu),
     .text={.slot=0},.font=KSN_BODY},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(20,52,220,64),.color=COLOR(0x8ef0c4ffu),
     .text={.slot=1},.font=KSN_BODY},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(20,66,220,78),.color=COLOR(0xa9bacaffu),
     .text={.slot=2},.font=KSN_BODY},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(20,80,220,92),.color=COLOR(0xffd479ffu),
     .text={.slot=3},.font=KSN_BODY},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(12,104,228,116),.color=COLOR(0x8fa6bcffu),
     .text={.slot=4},.font=KSN_BODY}
};
static const ksn_schema imucal={.version=1,.slot_count=5,.node_count=7,
    .background=0x071425ffu,.slots=imucal_slots,.nodes=imucal_nodes};

static const ksn_schema_slot bridge_slots[]={
    {"st",KSN_SLOT_TEXT,47,0,0},{"info",KSN_SLOT_TEXT,47,0,0},
    {"job",KSN_SLOT_TEXT,47,0,0},{"seen",KSN_SLOT_TEXT,47,0,0}
};
static const ksn_schema_node bridge_nodes[]={
    {.kind=KSN_NODE_TEXT,.bounds=RECT(10,8,230,18),.color=COLOR(0x7fd7ffffu),
     .text=TEXT("POCKET BRIDGE / PC LINK"),.font=KSN_CAPTION},
    {.kind=KSN_NODE_RECT,.bounds=RECT(8,24,232,98),.color=COLOR(0x14293cffu)},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(16,31,224,41),.color=COLOR(0xf5eedcffu),
     .text={.slot=0},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(16,49,224,59),.color=COLOR(0x9fb6c8ffu),
     .text={.slot=1},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(16,66,224,76),.color=COLOR(0x9fb6c8ffu),
     .text={.slot=2},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(16,83,224,93),.color=COLOR(0x8ef0c4ffu),
     .text={.slot=3},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(10,120,230,129),.color=COLOR(0xf5bb69ffu),
     .text=TEXT("ENTER SUBMIT   ESC HOME"),.font=KSN_CAPTION}
};
static const ksn_schema bridge={.version=1,.slot_count=4,.node_count=7,
    .background=0x08131fffu,.slots=bridge_slots,.nodes=bridge_nodes};

static const ksn_schema_slot companion_slots[]={
    {"head",KSN_SLOT_TEXT,47,0,0},{"line0",KSN_SLOT_TEXT,47,0,0},
    {"line1",KSN_SLOT_TEXT,47,0,0},{"line2",KSN_SLOT_TEXT,47,0,0},
    {"line3",KSN_SLOT_TEXT,47,0,0},{"foot",KSN_SLOT_TEXT,47,0,0},
    {"hint",KSN_SLOT_TEXT,47,0,0},{"variant",KSN_SLOT_U16,0,0,0},
    {"resource",KSN_SLOT_RESOURCE,0,0,0}
};
static const ksn_schema_node companion_nodes[]={
    {.kind=KSN_NODE_RECT,.bounds=RECT(8,27,98,103),.color=COLOR(0x203446ffu)},
    {.kind=KSN_NODE_IMAGE,.bounds=RECT(20,32,84,96),.resource={.slot=8},
     .variant={.slot=7},.frame={.slot=LIT,.literal.number=0},
     .source_width=64,.source_height=64},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(10,8,235,18),.color=COLOR(0x70e0d1ffu),
     .text={.slot=0},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(106,32,236,44),.color=COLOR(0xfbe8b8ffu),
     .text={.slot=1},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(106,50,236,62),.color=COLOR(0xbdcedbffu),
     .text={.slot=2},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(106,68,236,80),.color=COLOR(0xbdcedbffu),
     .text={.slot=3},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(106,86,236,98),.color=COLOR(0xbdcedbffu),
     .text={.slot=4},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(9,113,237,123),.color=COLOR(0xfbe8b8ffu),
     .text={.slot=5},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(9,125,237,133),.color=COLOR(0x89a4bffu),
     .text={.slot=6},.font=KSN_CAPTION}
};
static const ksn_schema companion={.version=1,.slot_count=9,.node_count=9,
    .background=0x091323ffu,.slots=companion_slots,.nodes=companion_nodes};

static const ksn_schema_slot pet_slots[]={
    {"title",KSN_SLOT_TEXT,47,0,0},{"index",KSN_SLOT_TEXT,5,0,0},
    {"species",KSN_SLOT_TEXT,12,0,0},{"status",KSN_SLOT_TEXT,16,0,0},
    {"food",KSN_SLOT_TEXT,16,0,0},{"joy",KSN_SLOT_TEXT,16,0,0},
    {"energy",KSN_SLOT_TEXT,16,0,0},{"foot",KSN_SLOT_TEXT,47,0,0},
    {"hint",KSN_SLOT_TEXT,47,0,0},{"note",KSN_SLOT_TEXT,47,0,0},
    {"resource",KSN_SLOT_RESOURCE,0,0,0},{"variant",KSN_SLOT_U16,0,0,11},
    {"frame",KSN_SLOT_U16,0,0,5},{"petY",KSN_SLOT_U16,0,0,71},
    {"bar0",KSN_SLOT_U16,0,1,106},{"bar1",KSN_SLOT_U16,0,1,106},
    {"bar2",KSN_SLOT_U16,0,1,106},{"ready",KSN_SLOT_BOOL,0,0,0},
    {"bubble",KSN_SLOT_BOOL,0,0,0},{"background",KSN_SLOT_COLOR,0,0,0},
    {"reveal",KSN_SLOT_U16,0,0,47}
};
static const ksn_schema_node pet_nodes[]={
    {.kind=KSN_NODE_TEXT,.bounds=RECT(9,7,184,21),.color=COLOR(0xf5eedcffu),
     .text={.slot=0},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(190,7,238,21),.color=COLOR(0x67dfc7ffu),
     .text={.slot=1},.font=KSN_CAPTION},
    {.kind=KSN_NODE_RECT,.bounds=RECT(7,25,112,109),.color=COLOR(0x1c3043ffu)},
    {.kind=KSN_NODE_RECT,.bounds=RECT(15,94,104,96),.color=COLOR(0x476275ffu)},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(15,99,111,108),.color=COLOR(0xb8c7d6ffu),
     .text={.slot=2},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(122,29,238,43),.color=COLOR(0x67dfc7ffu),
     .text={.slot=3},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(122,45,232,57),.color=COLOR(0xc9d5dfffu),
     .text={.slot=4},.font=KSN_CAPTION},
    {.kind=KSN_NODE_RECT,.bounds=RECT(122,56,122,59),.color=COLOR(0xf5bb69ffu),
     .rect_add_mask=KSN_SCHEMA_ADD_X1,.rect_add_slot={0,0,14,0}},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(122,63,232,75),.color=COLOR(0xc9d5dfffu),
     .text={.slot=5},.font=KSN_CAPTION},
    {.kind=KSN_NODE_RECT,.bounds=RECT(122,74,122,77),.color=COLOR(0xf08bbcffu),
     .rect_add_mask=KSN_SCHEMA_ADD_X1,.rect_add_slot={0,0,15,0}},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(122,81,232,93),.color=COLOR(0xc9d5dfffu),
     .text={.slot=6},.font=KSN_CAPTION},
    {.kind=KSN_NODE_RECT,.bounds=RECT(122,92,122,95),.color=COLOR(0x67dfc7ffu),
     .rect_add_mask=KSN_SCHEMA_ADD_X1,.rect_add_slot={0,0,16,0}},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(8,114,238,124),.color=COLOR(0xf5bb69ffu),
     .text={.slot=7},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(8,126,238,135),.color=COLOR(0x91a6baffu),
     .text={.slot=8},.font=KSN_CAPTION},
    {.kind=KSN_NODE_IMAGE,.bounds=RECT(27,0,91,64),.resource={.slot=10},
     .variant={.slot=11},.frame={.slot=12},.source_width=64,.source_height=64,
     .flags=KSN_SCHEMA_HAS_VISIBLE,.visible={.slot=17},
     .rect_add_mask=KSN_SCHEMA_ADD_Y0|KSN_SCHEMA_ADD_Y1,
     .rect_add_slot={0,13,0,13}},
    {.kind=KSN_NODE_RECT,.bounds=RECT(96,24,236,45),.color=COLOR(0x080c21ffu),
     .flags=KSN_SCHEMA_HAS_VISIBLE,.visible={.slot=18}},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(100,31,232,39),.color=COLOR(0x080c21ffu),
     .text={.slot=9},.font=KSN_CAPTION,
     .flags=KSN_SCHEMA_HAS_VISIBLE|KSN_SCHEMA_HAS_REVEAL,
     .visible={.slot=18},.reveal={.slot=20}}
};
static const ksn_schema pet={.version=1,.slot_count=21,.node_count=17,
    .background=0x0b1727ffu,.dynamic_background=true,.background_slot=19,
    .slots=pet_slots,.nodes=pet_nodes};

static const ksn_schema_slot clock_slots[]={
    {"face",KSN_SLOT_TEXT,47,0,0},{"tag",KSN_SLOT_TEXT,47,0,0}
};
static const ksn_schema_node clock_nodes[]={
    {.kind=KSN_NODE_RECT,.bounds=RECT(0,0,96,22),.color=COLOR(0x060c1affu)},
    {.kind=KSN_NODE_RECT,.bounds=RECT(0,0,96,1),.color=COLOR(0x3edcd0ffu)},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(4,4,48,16),.color=COLOR(0xe2f0ffffu),
     .text={.slot=0},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(50,12,94,24),.color=COLOR(0x63d6ddffu),
     .text={.slot=1},.font=KSN_CAPTION}
};
static const ksn_schema clock_view={.version=1,.slot_count=2,.node_count=4,
    .background=0x000000ffu,.slots=clock_slots,.nodes=clock_nodes};
typedef struct {
    uint64_t key,revision;
    uint32_t generation;
    bool initialized;
    char face[6],tag[8];
    ksn_schema_value fields[2];
} clock_source;
static ksn_result clock_acquire(void *opaque,uint64_t cursor,uint64_t now_us,
                                ksn_source_snapshot *out){
    clock_source *source=opaque;
    (void)cursor;(void)now_us;
    if(!source||!out||source->revision==UINT64_MAX)return KSN_INVALID;
    sys_clock_state clock;bool valid=sys_device_clock_read(&clock)&&
        clock.seconds<=INT64_C(9007199254740);
    uint16_t minute=0;bool sync=false;
    if(valid){
        int64_t second=clock.seconds+(clock.microseconds>=999500?1:0);
        int64_t day=second%86400;
        if(day<0)day+=86400;
        minute=(uint16_t)(day/60);
        sync=clock.source!=SYS_CLOCK_PC;
    }
    uint64_t key=valid?(uint64_t)minute+1u:0u;
    if(sync)key|=UINT64_C(1)<<16;
    if(!source->initialized||source->key!=key){
    if(valid)(void)snprintf(source->face,sizeof(source->face),"%02u:%02u",(unsigned)(minute/60),
                            (unsigned)(minute%60));
    else memcpy(source->face,"--:--",6);
    const char *label=sync?"UTC":"NO SYNC";
    size_t bytes=strlen(label);memcpy(source->tag,label,bytes+1u);
    /* Formatted directly into source-owned text buffers: these are bytes
     * materialized by the producer, not a second snapshot copy. */
    ksn_p0_probe_copy(KSN_P0_PRODUCER_MATERIALIZED,6u+bytes+1u);
    source->fields[0].data.text=(ksn_schema_text){source->face,5};
    source->fields[1].data.text=(ksn_schema_text){source->tag,(uint16_t)bytes};
    source->key=key;source->initialized=true;source->revision++;
    }
    *out=(ksn_source_snapshot){.size=sizeof(*out),.version=KSN_SOURCE_ABI_VERSION,
        .field_count=2,.generation=source->generation,
        .revision=source->revision,.valid_fields=3,.changed_fields=3,
        .fields=source->fields};
    return KSN_OK;
}
static void clock_release(void *opaque,const ksn_source_snapshot *snapshot){
    (void)opaque;(void)snapshot;
}
static bool clock_allow(void *opaque,uint32_t consumer){
    (void)opaque;return consumer!=0;
}
static ksn_result clock_open(void *storage,ksn_source_provider *out){
    static const ksn_slot_type types[]={KSN_SLOT_TEXT,KSN_SLOT_TEXT};
    if(!storage||!out)return KSN_INVALID;
    *out=(ksn_source_provider){.size=sizeof(*out),.version=KSN_SOURCE_ABI_VERSION,
        .field_count=2,.field_types=types,.context=storage,
        .acquire=clock_acquire,.release=clock_release,.allow=clock_allow};
    return KSN_OK;
}
static void clock_registered(void *storage,ksn_source_handle handle){
    ((clock_source *)storage)->generation=handle.generation;
}
static const ksn_source_binding clock_bindings[]={{0,0},{1,1}};
static const pocket_app_view_asset hello_asset={.schema=&hello};
static const pocket_app_view_asset imucal_asset={.schema=&imucal};
static const pocket_app_view_asset bridge_asset={.schema=&bridge};
static const pocket_app_view_asset companion_asset={.schema=&companion};
static const pocket_app_view_asset pet_asset={.schema=&pet};
static const pocket_app_view_asset clock_asset={.schema=&clock_view,
    .source_bytes=sizeof(clock_source),.source_open=clock_open,
    .source_registered=clock_registered,.source_bindings=clock_bindings,
    .source_binding_count=2};
const pocket_app_view_asset *pocket_app_view_lookup(const char *name){
    if(!name)return NULL;
    if(strcmp(name,"hello")==0)return &hello_asset;
    if(strcmp(name,"imucal")==0)return &imucal_asset;
    if(strcmp(name,"bridge")==0)return &bridge_asset;
    if(strcmp(name,"companion")==0)return &companion_asset;
    if(strcmp(name,"pet")==0)return &pet_asset;
    if(strcmp(name,"clock")==0)return &clock_asset;
    return NULL;
}
static const pocket_app_image_asset pet_image_asset={"pets",64,64,12,6,ksn_pet_builtin_image};
const pocket_app_image_asset *pocket_app_image_lookup(const char *name){
    return name&&strcmp(name,"pets")==0?&pet_image_asset:NULL;
}
