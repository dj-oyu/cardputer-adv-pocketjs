#include "ksn_source.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    ksn_source_snapshot snapshot;
    ksn_schema_value fields[2];
    uint64_t cursors[16];
    unsigned acquisitions,releases;
    bool allowed;
} fake_source;
static ksn_result acquire(void *context,uint64_t cursor,uint64_t now_us,
                          ksn_source_snapshot *out){
    fake_source *f=context;
    (void)now_us;
    assert(f->acquisitions<16);
    f->cursors[f->acquisitions++]=cursor;
    *out=f->snapshot;
    return KSN_OK;
}
static void release(void *context,const ksn_source_snapshot *snapshot){
    fake_source *f=context;
    assert(snapshot->fields==f->fields);
    f->releases++;
}
static bool allow(void *context,uint32_t consumer){
    return ((fake_source *)context)->allowed&&consumer==7u;
}

static void u32_source_test(void){
    static const ksn_slot_type types[]={KSN_SLOT_U32};
    static const ksn_schema_slot slots[]={
        {"frames",KSN_SLOT_U32,0,0,UINT32_MAX-1u}
    };
    const ksn_schema schema={.version=KSN_SCHEMA_ABI_VERSION,.slot_count=1,
        .background=0x000000ffu,.slots=slots};
    ksn_schema_value base[KSN_SCHEMA_MAX_SLOTS]={0};
    fake_source f={.allowed=true};
    f.fields[0].data.wide_number=UINT32_MAX-1u;
    f.snapshot=(ksn_source_snapshot){.size=sizeof(f.snapshot),
        .version=KSN_SOURCE_ABI_VERSION,.field_count=1,.revision=1,
        .valid_fields=1,.changed_fields=1,.fields=f.fields};
    ksn_source_provider provider={.size=sizeof(provider),
        .version=KSN_SOURCE_ABI_VERSION,.field_count=1,.field_types=types,
        .context=&f,.acquire=acquire,.release=release,.allow=allow};
    ksn_source_registry registry;
    ksn_source_registry_init(&registry);
    ksn_source_handle handle={0};
    assert(ksn_schema_validate(&schema)==KSN_OK);
    provider.version=1;
    assert(ksn_source_register(&registry,&provider,&handle)==KSN_INVALID);
    provider.version=KSN_SOURCE_ABI_VERSION;
    assert(ksn_source_register(&registry,&provider,&handle)==KSN_OK);
    f.snapshot.generation=handle.generation;
    ksn_source_subscription sub={0};
    const ksn_source_binding binding={0,0};
    assert(ksn_source_subscribe(&registry,handle,7,&schema,&binding,1,&sub)==KSN_OK);
    ksn_schema_value effective[KSN_SCHEMA_MAX_SLOTS]={0};
    ksn_source_lease lease={0};
    assert(ksn_source_acquire(&registry,&sub,&schema,base,0,effective,&lease)==KSN_OK);
    assert(effective[0].data.wide_number==UINT32_MAX-1u);
    assert(ksn_source_commit(&lease)==KSN_OK);
    ksn_source_release(&lease);
    f.snapshot.version=1;
    assert(ksn_source_acquire(&registry,&sub,&schema,base,0,effective,&lease)==KSN_INVALID);
    assert(sub.validated_revision==1);
    f.snapshot.version=KSN_SOURCE_ABI_VERSION;
    f.snapshot.revision=2;
    f.fields[0].data.wide_number=UINT32_MAX;
    assert(ksn_source_acquire(&registry,&sub,&schema,base,0,effective,&lease)==KSN_INVALID);
    assert(sub.validated_revision==1&&effective[0].data.wide_number==0);
    assert(ksn_source_unregister(&registry,handle)==KSN_OK);
    assert(f.acquisitions==f.releases);
    const ksn_schema_slot bad_u16={"bad",KSN_SLOT_U16,0,0,65536};
    const ksn_schema bad={.version=KSN_SCHEMA_ABI_VERSION,.slot_count=1,
        .background=0x000000ffu,.slots=&bad_u16};
    assert(ksn_schema_validate(&bad)==KSN_INVALID);
}

int main(void){
    u32_source_test();
    static const ksn_slot_type types[]={KSN_SLOT_TEXT,KSN_SLOT_U16};
    static const ksn_schema_slot slots[]={
        {"label",KSN_SLOT_TEXT,12,0,0},{"count",KSN_SLOT_U16,0,0,99}
    };
    const ksn_schema schema={.version=KSN_SCHEMA_ABI_VERSION,.slot_count=2,.background=0x000000ffu,
                              .slots=slots};
    ksn_schema_value base[KSN_SCHEMA_MAX_SLOTS]={0};
    base[0].data.text=(ksn_schema_text){"base",4};
    base[1].data.number=3;
    fake_source f={.allowed=true};
    f.fields[0].data.text=(ksn_schema_text){"native",6};
    f.fields[1].data.number=7;
    f.snapshot=(ksn_source_snapshot){.size=sizeof(f.snapshot),
        .version=KSN_SOURCE_ABI_VERSION,.field_count=2,.revision=1,
        .valid_fields=3,.changed_fields=3,.fields=f.fields};
    ksn_source_provider provider={.size=sizeof(provider),
        .version=KSN_SOURCE_ABI_VERSION,.field_count=2,.field_types=types,
        .context=&f,.acquire=acquire,.release=release,.allow=allow};
    ksn_source_registry registry;
    ksn_source_registry_init(&registry);
    ksn_source_handle handle={0};
    static const ksn_slot_type invalid_type[]={ (ksn_slot_type)-1 };
    ksn_source_provider invalid_provider=provider;
    invalid_provider.field_count=1;invalid_provider.field_types=invalid_type;
    assert(ksn_source_register(&registry,&invalid_provider,&handle)==KSN_INVALID);
    assert(ksn_source_register(&registry,&provider,&handle)==KSN_OK);
    f.snapshot.generation=handle.generation;
    const ksn_source_binding bindings[]={{0,0},{1,1}};
    ksn_source_subscription a={0},b={0};
    assert(ksn_source_subscribe(&registry,handle,8,&schema,bindings,2,&a)==KSN_UNSUPPORTED);
    assert(ksn_source_subscribe(&registry,handle,7,&schema,bindings,2,&a)==KSN_OK);
    assert(ksn_source_subscribe(&registry,handle,7,&schema,bindings,2,&b)==KSN_OK);
    const ksn_source_binding duplicate[]={{0,0},{0,0}};
    assert(ksn_source_subscribe(&registry,handle,7,&schema,duplicate,2,&b)==KSN_INVALID);
    assert(ksn_source_subscribe(&registry,handle,7,&schema,bindings,2,&b)==KSN_OK);
    ksn_schema_value effective[KSN_SCHEMA_MAX_SLOTS]={0};
    ksn_source_lease lease={0};
    assert(ksn_source_acquire(&registry,&a,&schema,base,10,base,&lease)==KSN_INVALID);
    assert(ksn_source_acquire(&registry,&a,&schema,base,10,effective,&lease)==KSN_OK);
    assert(effective[0].data.text.utf8==f.fields[0].data.text.utf8);
    assert(effective[1].data.number==7&&base[1].data.number==3);
    assert(lease.dirty_slots==3&&ksn_source_unregister(&registry,handle)==KSN_BUSY);
    /* Preflight failure: release without commit leaves the cursor at zero. */
    ksn_source_release(&lease);
    assert(ksn_source_acquire(&registry,&a,&schema,base,11,effective,&lease)==KSN_OK);
    assert(f.cursors[1]==0&&lease.dirty_slots==3);
    assert(ksn_source_commit(&lease)==KSN_OK);
    assert(ksn_source_commit(&lease)==KSN_INVALID);
    ksn_source_release(&lease);
    assert(a.validated_revision==1&&a.displayed_revision==0);
    assert(ksn_source_presented(&a,2)==KSN_INVALID);
    assert(ksn_source_presented(&a,1)==KSN_OK);
    assert(ksn_source_acquire(&registry,&b,&schema,base,12,effective,&lease)==KSN_OK);
    assert(f.cursors[2]==0&&lease.dirty_slots==3); /* independent reader */
    assert(ksn_source_commit(&lease)==KSN_OK);ksn_source_release(&lease);

    f.snapshot.revision=2;f.snapshot.changed_fields=1;
    f.fields[0].data.text=(ksn_schema_text){"new",3};
    assert(ksn_source_acquire(&registry,&a,&schema,base,13,effective,&lease)==KSN_OK);
    assert(f.cursors[3]==1&&lease.dirty_slots==1);
    assert(ksn_source_commit(&lease)==KSN_OK);ksn_source_release(&lease);
    assert(a.displayed_revision==1&&a.validated_revision==2);
    /* A skipped revision has no trustworthy one-step changed mask. */
    f.snapshot.revision=5;f.snapshot.changed_fields=2;
    assert(ksn_source_acquire(&registry,&a,&schema,base,14,effective,&lease)==KSN_OK);
    assert(lease.dirty_slots==3);
    assert(ksn_source_commit(&lease)==KSN_OK);ksn_source_release(&lease);

    /* Expiry reverts to the current JS base even if revision is unchanged. */
    f.snapshot.expires_at_us=20;
    assert(ksn_source_acquire(&registry,&a,&schema,base,20,effective,&lease)==KSN_OK);
    assert(effective[0].data.text.utf8==base[0].data.text.utf8&&
           effective[1].data.number==3&&lease.dirty_slots==3);
    assert(ksn_source_commit(&lease)==KSN_OK);ksn_source_release(&lease);

    /* Malformed producer data cannot advance the cursor or partially edit base. */
    f.snapshot.revision=6;f.snapshot.expires_at_us=0;
    f.snapshot.generation=handle.generation+1u;
    unsigned before=f.releases;
    assert(ksn_source_acquire(&registry,&a,&schema,base,21,effective,&lease)==KSN_INVALID);
    assert(f.releases==before+1&&a.validated_revision==5);
    f.snapshot.generation=handle.generation;
    f.snapshot.revision=4;
    assert(ksn_source_acquire(&registry,&a,&schema,base,21,effective,&lease)==KSN_INVALID);
    assert(a.validated_revision==5);
    f.snapshot.revision=6;
    f.fields[0].data.text=(ksn_schema_text){"too-long-for-the-slot",21};
    before=f.releases;
    assert(ksn_source_acquire(&registry,&a,&schema,base,21,effective,&lease)==KSN_INVALID);
    assert(f.releases==before+1&&a.validated_revision==5&&
           strcmp(base[0].data.text.utf8,"base")==0&&
           effective[0].data.text.utf8==base[0].data.text.utf8&&
           effective[1].data.number==base[1].data.number);
    f.fields[0].data.text=(ksn_schema_text){"ok",2};
    f.fields[1].data.number=100; /* First field valid, second invalid. */
    before=f.releases;
    assert(ksn_source_acquire(&registry,&a,&schema,base,21,effective,&lease)==KSN_INVALID);
    assert(f.releases==before+1&&a.validated_revision==5&&
           effective[0].data.text.utf8==base[0].data.text.utf8&&
           effective[1].data.number==base[1].data.number);
    f.fields[1].data.number=7;
    f.allowed=false;
    assert(ksn_source_acquire(&registry,&a,&schema,base,22,effective,&lease)==KSN_UNSUPPORTED);
    f.allowed=true;
    assert(ksn_source_unregister(&registry,handle)==KSN_OK);
    assert(ksn_source_acquire(&registry,&a,&schema,base,23,effective,&lease)==KSN_STALE);
    ksn_source_handle next={0};
    assert(ksn_source_register(&registry,&provider,&next)==KSN_OK);
    assert(next.index==handle.index&&next.generation!=handle.generation);
    assert(ksn_source_unregister(&registry,next)==KSN_OK);
    ksn_source_registry_init(&registry);
    ksn_source_handle rebuilt={0};
    assert(ksn_source_register(&registry,&provider,&rebuilt)==KSN_OK);
    assert(rebuilt.index==handle.index&&rebuilt.generation!=handle.generation&&
           rebuilt.generation!=next.generation);
    assert(ksn_source_acquire(&registry,&a,&schema,base,24,effective,&lease)==KSN_STALE);
    assert(f.acquisitions==f.releases);
    puts("generic source: PASS (capability, typed binding, pin, cursor, expiry, malformed, multi-reader)");
    return 0;
}
