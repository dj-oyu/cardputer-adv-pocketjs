#include "use_cases.h"
/* Recording double only: not a renderer, reference validator or allocator. */
typedef struct {
    bool busy,building;
    ds_layer layer;
    unsigned calls,fail_at,aborts,commits,draws[2],pending,changes;
    ds_rect last_rect;
    char text[32];uint16_t bytes;
    bool captured;unsigned releases;
} probe;
static ds_result operation(probe *p){return ++p->calls==p->fail_at?DS_OOM:DS_OK;}
static ds_result begin(void *v,ds_update_mode mode,ds_tx *tx){
    probe *p=v;if(p->busy||p->building)return DS_BUSY;
    p->building=true;p->pending=mode==DS_REPLACE?0:p->draws[p->layer];
    tx->value=1;return DS_OK;
}
static ds_result background(void *v,ds_tx tx,ds_rgba color){(void)tx;(void)color;return operation(v);}
static ds_result add(void *v,ds_tx tx,const ds_draw *d,ds_ref *ref){
    (void)tx;probe *p=v;ds_result r=operation(p);if(r!=DS_OK)return r;
    if(d->kind==DS_TEXT&&d->data.text.bytes>d->data.text.capacity)return DS_LIMIT;
    ref->value=++p->pending;return DS_OK;
}
static ds_result change(void *v,ds_tx tx,ds_ref ref,const ds_change *c){
    (void)tx;(void)ref;probe *p=v;ds_result r=operation(p);if(r!=DS_OK)return r;
    ++p->changes;
    if(c->property==DS_SET_RECT)p->last_rect=c->value.rect;
    if(c->property==DS_SET_TEXT){
        if(c->value.text.bytes>sizeof(p->text))return DS_LIMIT;
        p->bytes=c->value.text.bytes;
        for(unsigned i=0;i<p->bytes;i++)p->text[i]=c->value.text.utf8[i];
    }
    return DS_OK;
}
static ds_result animate(void *v,ds_tx tx,const ds_motion *m,ds_animation *a){
    (void)v;(void)tx;(void)m;(void)a;return DS_UNSUPPORTED;
}
static ds_result stop(void *v,ds_tx tx,ds_animation a){(void)v;(void)tx;(void)a;return DS_UNSUPPORTED;}
static ds_result end(void *v,ds_tx tx){
    (void)tx;probe *p=v;ds_result r=operation(p);if(r!=DS_OK)return r;
    p->draws[p->layer]=p->pending;p->building=false;++p->commits;return DS_OK;
}
static void abort_tx(void *v,ds_tx tx){(void)tx;probe *p=v;p->building=false;++p->aborts;}
static ds_limits limits(void *v){(void)v;return (ds_limits){{80,896,6},{16,128,2},16384,2048};}
static ds_stats stats(void *v){(void)v;return (ds_stats){0};}
static ds_result capture(void *v,const ds_backdrop_request *r,ds_backdrop_mode *actual){
    probe *p=v;(void)r;if(p->busy)return DS_BUSY;
    p->captured=true;*actual=DS_BACKDROP_SOLID;return DS_OK;
}
static ds_result release(void *v){probe *p=v;p->captured=false;++p->releases;return DS_OK;}
static const ds_api api={begin,background,add,change,animate,stop,end,abort_tx,limits,stats};
static const ds_backdrop_api backdrop={capture,release};
#define CHECK(x) do { if(!(x))return __LINE__; } while(0)
int contract_test(void){
    probe p={.layer=DS_APP};ds_client c={&api,&p};pet_view view={{99},{99},{99}};
    CHECK(pet_view_build(c,(ds_resource){1},&view)==DS_OK);
    CHECK(p.draws[DS_APP]==3&&p.commits==1&&view.label.value==3);
    CHECK(pet_view_update(c,&view,100)==DS_OK);
    CHECK(p.last_rect.x1==228&&p.bytes==8&&p.text[5]=='1'&&p.text[7]=='0');
    CHECK(pet_view_update(c,&view,0)==DS_OK&&p.last_rect.x1==122&&p.bytes==6);
    CHECK(pet_view_update(c,&view,101)==DS_INVALID);
    p.layer=DS_SYSTEM;CHECK(notice_view_show(c,"ALARM",5)==DS_OK);p.layer=DS_APP;
    CHECK(p.draws[DS_APP]==3&&p.draws[DS_SYSTEM]==1);
    p.busy=true;unsigned commits=p.commits;
    CHECK(pet_view_update(c,&view,80)==DS_BUSY&&p.commits==commits);
    p.busy=false;
    /* Every build operation, including end, can fail without publishing refs. */
    for(unsigned failure=1;failure<=5;failure++){
        p=(probe){.layer=DS_APP,.fail_at=failure};view=(pet_view){{99},{99},{99}};
        CHECK(pet_view_build(c,(ds_resource){1},&view)==DS_OOM);
        CHECK(view.pet.value==99&&p.aborts==1&&p.commits==0&&!p.building);
    }
    p=(probe){.layer=DS_APP};ds_backdrop_mode actual=DS_BACKDROP_FROSTED;
    CHECK(modal_view_open(c,&backdrop,&actual)==DS_OK&&actual==DS_BACKDROP_SOLID);
    CHECK(p.captured&&p.draws[DS_APP]==1);
    p=(probe){.layer=DS_APP,.fail_at=1};
    CHECK(modal_view_open(c,&backdrop,&actual)==DS_OOM);
    CHECK(!p.captured&&p.releases==1&&p.aborts==1);
    p=(probe){.layer=DS_APP,.busy=true};
    CHECK(modal_view_open(c,&backdrop,&actual)==DS_BUSY&&p.commits==0);
    return 0;
}
#ifdef DS_CONTRACT_HOST
#include <stdio.h>
int main(void){
    int line=contract_test();
    if(line){fprintf(stderr,"contract failure at line %d\n",line);return 1;}
    puts("contract examples: PASS (recording double, not renderer)");return 0;
}
#endif
