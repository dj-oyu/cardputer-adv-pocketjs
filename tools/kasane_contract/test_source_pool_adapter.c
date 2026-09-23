#include "ksn_source_pool_adapter.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    ksn_schema_value fields[2];
    char text[16];
    uint64_t expires_at_us;
    uint32_t changed_fields;
    bool malformed;
} payload;
static ksn_result describe(const void *data,ksn_source_pool_view *out){
    const payload *p=data;
    *out=(ksn_source_pool_view){.fields=p->fields+(p->malformed?1:0),
        .valid_fields=3,.changed_fields=p->changed_fields,
        .expires_at_us=p->expires_at_us};
    return KSN_OK;
}
static bool allow(void *policy,uint32_t consumer){
    return *(const bool *)policy&&consumer==7;
}
static ksn_result publish(ksn_source_pool *pool,const char *text,
                          uint16_t count,uint64_t expires,bool malformed){
    ksn_source_write write={0};
    ksn_result r=ksn_source_pool_begin(pool,&write);
    if(r!=KSN_OK)return r;
    payload *p=write.data;
    size_t bytes=strlen(text);
    assert(bytes<sizeof(p->text));
    memcpy(p->text,text,bytes+1);
    p->fields[0].data.text=(ksn_schema_text){p->text,(uint16_t)bytes};
    p->fields[1].data.number=count;
    p->expires_at_us=expires;p->changed_fields=3;p->malformed=malformed;
    return ksn_source_pool_publish(&write,NULL);
}
int main(void){
    payload slots[KSN_SOURCE_POOL_SLOTS]={0};
    ksn_source_pool pool={0};
    assert(ksn_source_pool_init(&pool,slots,sizeof(slots),sizeof(slots[0]))==KSN_OK);
    static const ksn_slot_type types[]={KSN_SLOT_TEXT,KSN_SLOT_U16};
    static const ksn_schema_slot schema_slots[]={
        {"label",KSN_SLOT_TEXT,15,0,0},{"count",KSN_SLOT_U16,0,0,99}
    };
    const ksn_schema schema={.version=1,.slot_count=2,.slots=schema_slots,
                             .background=0x000000ffu};
    bool permitted=true;
    ksn_source_pool_adapter adapter={0};ksn_source_provider provider={0};
    assert(ksn_source_pool_adapter_open(&adapter,&pool,types,2,1,
        describe,allow,&permitted,&provider)==KSN_INVALID);
    assert(ksn_source_pool_adapter_open(&adapter,&pool,types,2,
        offsetof(payload,fields),describe,allow,&permitted,&provider)==KSN_OK);
    ksn_source_registry registry;ksn_source_registry_init(&registry);
    ksn_source_handle handle={0};
    assert(ksn_source_register(&registry,&provider,&handle)==KSN_OK);
    assert(ksn_source_pool_adapter_registered(&adapter,handle)==KSN_OK);
    const ksn_source_binding bindings[]={{0,0},{1,1}};
    ksn_source_subscription a={0},b={0};
    assert(ksn_source_subscribe(&registry,handle,8,&schema,bindings,2,&a)==KSN_UNSUPPORTED);
    assert(ksn_source_subscribe(&registry,handle,7,&schema,bindings,2,&a)==KSN_OK);
    assert(ksn_source_subscribe(&registry,handle,7,&schema,bindings,2,&b)==KSN_OK);
    ksn_schema_value base[2]={0},effective[KSN_SCHEMA_MAX_SLOTS]={0};
    base[0].data.text=(ksn_schema_text){"base",4};base[1].data.number=9;
    ksn_source_lease la={0},lb={0},lc={0};
    assert(ksn_source_acquire(&registry,&a,&schema,base,0,effective,&la)==KSN_STALE);
    assert(publish(&pool,"alpha",1,0,false)==KSN_OK);
    assert(ksn_source_acquire(&registry,&a,&schema,base,1,effective,&la)==KSN_OK);
    const char *alpha=effective[0].data.text.utf8;
    assert(alpha==la.snapshot.fields[0].data.text.utf8);
    assert(strcmp(alpha,"alpha")==0&&effective[1].data.number==1);
    assert(ksn_source_acquire(&registry,&b,&schema,base,1,effective,&lb)==KSN_OK);
    assert(lb.snapshot.fields==la.snapshot.fields&&
           ksn_source_unregister(&registry,handle)==KSN_BUSY);
    assert(publish(&pool,"bravo",2,0,false)==KSN_OK);
    assert(strcmp(alpha,"alpha")==0);
    assert(ksn_source_commit(&la)==KSN_OK);
    ksn_source_release(&la);
    assert(ksn_source_acquire(&registry,&a,&schema,base,2,effective,&la)==KSN_OK);
    const char *bravo=effective[0].data.text.utf8;
    assert(strcmp(bravo,"bravo")==0&&la.dirty_slots==3);
    assert(publish(&pool,"charlie",3,0,false)==KSN_OK);
    assert(publish(&pool,"delta",4,0,false)==KSN_BUSY);
    assert(ksn_source_pool_skipped(&pool)==1&&strcmp(alpha,"alpha")==0&&
           strcmp(bravo,"bravo")==0);
    ksn_source_release(&lb);
    assert(publish(&pool,"delta",4,0,false)==KSN_OK);
    assert(strcmp(bravo,"bravo")==0);
    ksn_source_release(&la);
    assert(ksn_source_acquire(&registry,&b,&schema,base,3,effective,&lc)==KSN_OK);
    assert(lc.snapshot.revision==4&&strcmp(effective[0].data.text.utf8,"delta")==0);
    assert(ksn_source_commit(&lc)==KSN_OK);
    ksn_source_release(&lc);
    assert(publish(&pool,"expired",5,10,false)==KSN_OK);
    assert(ksn_source_acquire(&registry,&b,&schema,base,10,effective,&lc)==KSN_OK);
    assert(effective[0].data.text.utf8==base[0].data.text.utf8&&
           effective[1].data.number==9&&lc.dirty_slots==3);
    ksn_source_release(&lc);
    assert(publish(&pool,"bad",6,0,true)==KSN_OK);
    assert(ksn_source_acquire(&registry,&b,&schema,base,11,effective,&lc)==KSN_INVALID);
    assert(publish(&pool,"recovered",7,0,false)==KSN_OK);
    assert(ksn_source_acquire(&registry,&b,&schema,base,12,effective,&lc)==KSN_OK);
    assert(strcmp(effective[0].data.text.utf8,"recovered")==0);
    ksn_source_release(&lc);
    permitted=false;
    assert(ksn_source_acquire(&registry,&b,&schema,base,13,effective,&lc)==KSN_UNSUPPORTED);
    permitted=true;
    assert(ksn_source_unregister(&registry,handle)==KSN_OK);
    assert(ksn_source_acquire(&registry,&b,&schema,base,14,effective,&lc)==KSN_STALE);
    puts("source pool adapter: PASS (multi-consumer, zero-copy pin, exhaustion, expiry, invalid release)");
    return 0;
}
