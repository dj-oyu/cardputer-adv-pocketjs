#include "use_cases.h"
/* Recording double only: not a renderer, reference validator or allocator. */
typedef struct {
    bool busy,building;
    ksn_layer layer;
    unsigned calls,fail_at,aborts,commits,draws[2],pending,changes;
    ksn_rect last_rect;
    char text[32];uint16_t bytes;
    bool captured;unsigned releases;
} probe;
static ksn_result operation(probe *p){return ++p->calls==p->fail_at?KSN_OOM:KSN_OK;}
static ksn_result begin(void *v,ksn_update_mode mode,ksn_tx *tx){
    probe *p=v;if(p->busy||p->building)return KSN_BUSY;
    p->building=true;p->pending=mode==KSN_REPLACE?0:p->draws[p->layer];
    tx->value=1;return KSN_OK;
}
static ksn_result background(void *v,ksn_tx tx,ksn_rgba color){(void)tx;(void)color;return operation(v);}
static ksn_result add(void *v,ksn_tx tx,const ksn_draw *d,ksn_ref *ref){
    (void)tx;probe *p=v;ksn_result r=operation(p);if(r!=KSN_OK)return r;
    if(d->kind==KSN_TEXT&&d->data.text.bytes>d->data.text.capacity)return KSN_LIMIT;
    ref->value=++p->pending;return KSN_OK;
}
static ksn_result change(void *v,ksn_tx tx,ksn_ref ref,const ksn_change *c){
    (void)tx;(void)ref;probe *p=v;ksn_result r=operation(p);if(r!=KSN_OK)return r;
    ++p->changes;
    if(c->property==KSN_SET_RECT)p->last_rect=c->value.rect;
    if(c->property==KSN_SET_TEXT){
        if(c->value.text.bytes>sizeof(p->text))return KSN_LIMIT;
        p->bytes=c->value.text.bytes;
        for(unsigned i=0;i<p->bytes;i++)p->text[i]=c->value.text.utf8[i];
    }
    return KSN_OK;
}
static ksn_result animate(void *v,ksn_tx tx,const ksn_motion *m,ksn_animation *a){
    (void)v;(void)tx;(void)m;(void)a;return KSN_UNSUPPORTED;
}
static ksn_result stop(void *v,ksn_tx tx,ksn_animation a){(void)v;(void)tx;(void)a;return KSN_UNSUPPORTED;}
static ksn_result end(void *v,ksn_tx tx){
    (void)tx;probe *p=v;ksn_result r=operation(p);if(r!=KSN_OK)return r;
    p->draws[p->layer]=p->pending;p->building=false;++p->commits;return KSN_OK;
}
static void abort_tx(void *v,ksn_tx tx){(void)tx;probe *p=v;p->building=false;++p->aborts;}
static ksn_limits limits(void *v){(void)v;return (ksn_limits){{80,896,6},{16,128,2},16384,2048};}
static ksn_stats stats(void *v){(void)v;return (ksn_stats){0};}
static ksn_result capture(void *v,const ksn_backdrop_request *r,ksn_backdrop_mode *actual){
    probe *p=v;(void)r;if(p->busy)return KSN_BUSY;
    p->captured=true;*actual=KSN_BACKDROP_SOLID;return KSN_OK;
}
static ksn_result release(void *v){probe *p=v;p->captured=false;++p->releases;return KSN_OK;}
static const ksn_api api={begin,background,add,change,animate,stop,end,abort_tx,limits,stats};
static const ksn_backdrop_api backdrop={capture,release};
#define CHECK(x) do { if(!(x))return __LINE__; } while(0)
int contract_test(void){
    probe p={.layer=KSN_APP};ksn_client c={&api,&p};pet_view view={{99},{99},{99}};
    CHECK(pet_view_build(c,(ksn_resource){1},&view)==KSN_OK);
    CHECK(p.draws[KSN_APP]==3&&p.commits==1&&view.label.value==3);
    CHECK(pet_view_update(c,&view,100)==KSN_OK);
    CHECK(p.last_rect.x1==228&&p.bytes==8&&p.text[5]=='1'&&p.text[7]=='0');
    CHECK(pet_view_update(c,&view,0)==KSN_OK&&p.last_rect.x1==122&&p.bytes==6);
    CHECK(pet_view_update(c,&view,101)==KSN_INVALID);
    p.layer=KSN_SYSTEM;CHECK(notice_view_show(c,"ALARM",5)==KSN_OK);p.layer=KSN_APP;
    CHECK(p.draws[KSN_APP]==3&&p.draws[KSN_SYSTEM]==1);
    p.busy=true;unsigned commits=p.commits;
    CHECK(pet_view_update(c,&view,80)==KSN_BUSY&&p.commits==commits);
    p.busy=false;
    /* Every build operation, including end, can fail without publishing refs. */
    for(unsigned failure=1;failure<=5;failure++){
        p=(probe){.layer=KSN_APP,.fail_at=failure};view=(pet_view){{99},{99},{99}};
        CHECK(pet_view_build(c,(ksn_resource){1},&view)==KSN_OOM);
        CHECK(view.pet.value==99&&p.aborts==1&&p.commits==0&&!p.building);
    }
    p=(probe){.layer=KSN_APP};ksn_backdrop_mode actual=KSN_BACKDROP_FROSTED;
    CHECK(modal_view_open(c,&backdrop,&actual)==KSN_OK&&actual==KSN_BACKDROP_SOLID);
    CHECK(p.captured&&p.draws[KSN_APP]==1);
    p=(probe){.layer=KSN_APP,.fail_at=1};
    CHECK(modal_view_open(c,&backdrop,&actual)==KSN_OOM);
    CHECK(!p.captured&&p.releases==1&&p.aborts==1);
    p=(probe){.layer=KSN_APP,.busy=true};
    CHECK(modal_view_open(c,&backdrop,&actual)==KSN_BUSY&&p.commits==0);
    return 0;
}
#ifdef KSN_CONTRACT_HOST
#include <stdio.h>
int main(void){
    int line=contract_test();
    if(line){fprintf(stderr,"contract failure at line %d\n",line);return 1;}
    puts("contract examples: PASS (recording double, not renderer)");return 0;
}
#endif
