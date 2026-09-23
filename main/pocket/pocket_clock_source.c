#include "pocket_clock_source.h"
#include "ui/kasane/ksn_p0_probe.h"
#include "system/sys_device.h"
#include <stdio.h>
#include <string.h>

static ksn_result clock_acquire(void *opaque,uint64_t cursor,uint64_t now_us,
                                ksn_source_snapshot *out){
    pocket_clock_source_state *source=opaque;
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
        if(valid)(void)snprintf(source->face,sizeof(source->face),"%02u:%02u",
                                (unsigned)(minute/60),(unsigned)(minute%60));
        else memcpy(source->face,"--:--",6);
        const char *label=sync?"UTC":"NO SYNC";
        size_t bytes=strlen(label);memcpy(source->tag,label,bytes+1u);
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
ksn_result pocket_clock_source_open(void *storage,ksn_source_provider *out){
    static const ksn_slot_type types[]={KSN_SLOT_TEXT,KSN_SLOT_TEXT};
    if(!storage||!out)return KSN_INVALID;
    *out=(ksn_source_provider){.size=sizeof(*out),.version=KSN_SOURCE_ABI_VERSION,
        .field_count=2,.field_types=types,.context=storage,
        .acquire=clock_acquire,.release=clock_release,.allow=clock_allow};
    return KSN_OK;
}
void pocket_clock_source_registered(void *storage,ksn_source_handle handle){
    ((pocket_clock_source_state *)storage)->generation=handle.generation;
}
