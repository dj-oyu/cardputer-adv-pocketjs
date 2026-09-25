#include "ksn_source.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
_Static_assert(sizeof(ksn_source_bundle)<=512u,"bundle stays stack-bounded");

typedef struct {
    ksn_schema_value field;
    ksn_source_snapshot snapshot;
    uint64_t last_cursor;
    unsigned acquisitions,releases;
    bool allowed;
} fake;
static ksn_result acquire(void *context,uint64_t cursor,uint64_t now_us,
                          ksn_source_snapshot *out){
    fake *f=context;(void)now_us;
    f->last_cursor=cursor;f->acquisitions++;
    *out=f->snapshot;return KSN_OK;
}
static void release(void *context,const ksn_source_snapshot *snapshot){
    fake *f=context;assert(snapshot->fields==&f->field);f->releases++;
}
static bool allow(void *context,uint32_t consumer){
    return ((fake *)context)->allowed&&consumer==7;
}
static void prepare(fake *f,ksn_source_handle handle){
    f->snapshot=(ksn_source_snapshot){.size=sizeof(f->snapshot),
        .version=KSN_SOURCE_ABI_VERSION,.field_count=1,
        .generation=handle.generation,.revision=1,
        .valid_fields=1,.changed_fields=1,.fields=&f->field};
}
int main(void){
    static const ksn_schema_slot slots[]={
        {"label",KSN_SLOT_TEXT,15,0,0},{"count",KSN_SLOT_U16,0,0,99}
    };
    const ksn_schema schema={.version=KSN_SCHEMA_ABI_VERSION,.slot_count=2,.slots=slots,
                             .background=0x000000ffu};
    ksn_schema_value base[KSN_SCHEMA_MAX_SLOTS]={0},effective[KSN_SCHEMA_MAX_SLOTS]={0};
    base[0].data.text=(ksn_schema_text){"base",4};base[1].data.number=9;
    fake a={.allowed=true},b={.allowed=true};
    a.field.data.text=(ksn_schema_text){"alpha",5};b.field.data.number=7;
    static const ksn_slot_type text_type[]={KSN_SLOT_TEXT};
    static const ksn_slot_type number_type[]={KSN_SLOT_U16};
    ksn_source_provider pa={.size=sizeof(pa),.version=KSN_SOURCE_ABI_VERSION,
        .field_count=1,.field_types=text_type,.context=&a,
        .acquire=acquire,.release=release,.allow=allow};
    ksn_source_provider pb={.size=sizeof(pb),.version=KSN_SOURCE_ABI_VERSION,
        .field_count=1,.field_types=number_type,.context=&b,
        .acquire=acquire,.release=release,.allow=allow};
    ksn_source_registry ra,rb;ksn_source_registry_init(&ra);ksn_source_registry_init(&rb);
    ksn_source_handle ha={0},hb={0};
    assert(ksn_source_register(&ra,&pa,&ha)==KSN_OK);
    assert(ksn_source_register(&rb,&pb,&hb)==KSN_OK);
    prepare(&a,ha);prepare(&b,hb);
    const ksn_source_binding ba={0,0},bb={1,0};
    ksn_source_subscription sa={0},sb={0},duplicate={0};
    assert(ksn_source_subscribe(&ra,ha,7,&schema,&ba,1,&sa)==KSN_OK);
    assert(ksn_source_subscribe(&rb,hb,7,&schema,&bb,1,&sb)==KSN_OK);
    assert(ksn_source_subscribe(&ra,ha,7,&schema,&ba,1,&duplicate)==KSN_OK);
    const ksn_source_member members[]={{&ra,&sa},{&rb,&sb}};
    const ksn_source_member overlap[]={{&ra,&sa},{&rb,&sb},{&ra,&duplicate}};
    ksn_source_bundle bundle={0};
    assert(ksn_source_bundle_acquire(overlap,3,&schema,base,1,effective,&bundle)==KSN_INVALID);
    assert(a.acquisitions==0&&b.acquisitions==0);
    assert(ksn_source_bundle_acquire(members,2,&schema,base,1,effective,&bundle)==KSN_OK);
    assert(bundle.active&&bundle.count==2&&bundle.dirty_slots==3);
    assert(effective[0].data.text.utf8==a.field.data.text.utf8&&
           effective[1].data.number==7&&
           ksn_source_unregister(&ra,ha)==KSN_BUSY&&
           ksn_source_unregister(&rb,hb)==KSN_BUSY);
    ksn_source_bundle_release(&bundle); /* Preflight rejected: no cursor advance. */
    assert(!bundle.active&&bundle.count==0&&sa.validated_revision==0&&sb.validated_revision==0);
    assert(ksn_source_bundle_acquire(members,2,&schema,base,2,effective,&bundle)==KSN_OK);
    assert(a.last_cursor==0&&b.last_cursor==0);
    assert(ksn_source_bundle_commit(&bundle)==KSN_OK);
    assert(ksn_source_bundle_commit(&bundle)==KSN_INVALID);
    ksn_source_bundle_release(&bundle);
    assert(sa.validated_revision==1&&sb.validated_revision==1&&
           a.acquisitions==a.releases&&b.acquisitions==b.releases);

    a.snapshot.revision=2;b.snapshot.revision=2;
    a.field.data.text=(ksn_schema_text){"bravo",5};b.field.data.number=100;
    unsigned ar=a.releases,br=b.releases;
    assert(ksn_source_bundle_acquire(members,2,&schema,base,3,effective,&bundle)==KSN_INVALID);
    assert(!bundle.active&&bundle.count==0&&a.releases==ar+1&&b.releases==br+1);
    assert(sa.validated_revision==1&&sb.validated_revision==1&&
           effective[0].data.text.utf8==base[0].data.text.utf8&&
           effective[1].data.number==base[1].data.number);
    b.field.data.number=8;b.allowed=false;
    ar=a.releases;
    assert(ksn_source_bundle_acquire(members,2,&schema,base,4,effective,&bundle)==KSN_UNSUPPORTED);
    assert(a.releases==ar+1&&a.acquisitions==a.releases&&
           effective[0].data.text.utf8==base[0].data.text.utf8);
    b.allowed=true;
    assert(ksn_source_bundle_acquire(members,2,&schema,base,5,effective,&bundle)==KSN_OK);
    assert(a.last_cursor==1&&b.last_cursor==1&&bundle.dirty_slots==3);
    assert(ksn_source_bundle_commit(&bundle)==KSN_OK);ksn_source_bundle_release(&bundle);
    assert(sa.validated_revision==2&&sb.validated_revision==2);

    b.snapshot.expires_at_us=10;
    assert(ksn_source_bundle_acquire(members,2,&schema,base,10,effective,&bundle)==KSN_OK);
    assert(effective[0].data.text.utf8==a.field.data.text.utf8&&
           effective[1].data.number==base[1].data.number&&bundle.dirty_slots==2);
    assert(ksn_source_bundle_commit(&bundle)==KSN_OK);ksn_source_bundle_release(&bundle);
    assert(ksn_source_unregister(&rb,hb)==KSN_OK);
    ar=a.releases;
    assert(ksn_source_bundle_acquire(members,2,&schema,base,11,effective,&bundle)==KSN_STALE);
    assert(a.releases==ar+1&&a.acquisitions==a.releases&&
           effective[0].data.text.utf8==base[0].data.text.utf8);
    assert(ksn_source_unregister(&ra,ha)==KSN_OK);
    printf("source bundle: PASS (%zu bytes, disjoint registries, zero payload copy, atomic failure, cursors, expiry)\n",
           sizeof(bundle));
    return 0;
}
