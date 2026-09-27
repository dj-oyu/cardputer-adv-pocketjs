#include "ksn_source_pool_adapter.h"
#include <stdint.h>

static ksn_result pooled_acquire(void *context,uint64_t cursor,uint64_t now_us,
                                 ksn_source_snapshot *out){
    (void)cursor;(void)now_us;
    ksn_source_pool_adapter *a=context;
    if(!a||!out||!a->generation)return KSN_INVALID;
    ksn_source_read read={0};
    ksn_result r=ksn_source_pool_acquire(a->pool,&read);
    if(r!=KSN_OK)return r;
    ksn_source_pool_view view={0};
    r=a->describe(read.data,&view);
    const void *expected=(const unsigned char *)read.data+a->fields_offset;
    if(r!=KSN_OK||view.fields!=expected){
        (void)ksn_source_pool_release(&read);
        return r==KSN_OK?KSN_INVALID:r;
    }
    *out=(ksn_source_snapshot){.size=sizeof(*out),.version=KSN_SOURCE_ABI_VERSION,
        .field_count=a->field_count,.generation=a->generation,
        .revision=read.revision,.expires_at_us=view.expires_at_us,
        .valid_fields=view.valid_fields,.changed_fields=view.changed_fields,
        .fields=view.fields};
    return KSN_OK;
}
static void pooled_release(void *context,const ksn_source_snapshot *snapshot){
    ksn_source_pool_adapter *a=context;
    if(!a||!snapshot||!snapshot->fields)return;
    /* The borrowed field pointer identifies its pinned slot. No cookie or
     * pool-specific pointer layout enters the source snapshot ABI. */
    for(unsigned i=0;i<KSN_SOURCE_POOL_SLOTS;i++){
        const void *fields=a->pool->storage+a->pool->stride*i+a->fields_offset;
        if(snapshot->fields!=fields)continue;
        ksn_source_read read={.pool=a->pool,.data=a->pool->storage+a->pool->stride*i,
            .revision=(uint32_t)snapshot->revision,.slot=(uint8_t)i,.active=true};
        (void)ksn_source_pool_release(&read);
        return;
    }
}
static bool pooled_allow(void *context,uint32_t consumer){
    ksn_source_pool_adapter *a=context;
    return a&&a->allow(a->policy,consumer);
}
ksn_result ksn_source_pool_adapter_open(ksn_source_pool_adapter *adapter,
    ksn_source_pool *pool,const ksn_slot_type *field_types,uint8_t field_count,
    size_t fields_offset,ksn_source_pool_describe describe,
    bool (*allow)(void *policy,uint32_t consumer),void *policy,
    ksn_source_provider *provider){
    if(!adapter||!pool||!pool->storage||!field_types||!field_count||
       field_count>KSN_SOURCE_MAX_FIELDS||!describe||!allow||!provider||
       fields_offset>=pool->stride||
       fields_offset%_Alignof(ksn_schema_value)!=0||
       (size_t)field_count>(pool->stride-fields_offset)/sizeof(ksn_schema_value))
        return KSN_INVALID;
    for(unsigned i=0;i<field_count;i++)
        if((unsigned)field_types[i]>KSN_SLOT_U32)return KSN_INVALID;
    *adapter=(ksn_source_pool_adapter){.pool=pool,.field_types=field_types,
        .field_count=field_count,.fields_offset=fields_offset,
        .describe=describe,.allow=allow,.policy=policy};
    *provider=(ksn_source_provider){.size=sizeof(*provider),
        .version=KSN_SOURCE_ABI_VERSION,.field_count=field_count,
        .field_types=field_types,.context=adapter,
        .acquire=pooled_acquire,.release=pooled_release,.allow=pooled_allow};
    return KSN_OK;
}
ksn_result ksn_source_pool_adapter_registered(ksn_source_pool_adapter *adapter,
                                               ksn_source_handle handle){
    if(!adapter||!adapter->pool||!handle.generation||
       handle.index>=KSN_SOURCE_MAX_REGISTERED||adapter->generation)return KSN_INVALID;
    adapter->generation=handle.generation;
    return KSN_OK;
}
