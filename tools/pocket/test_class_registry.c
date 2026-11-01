/* Compile the extracted production implementation, never a hand-written
 * class-ready substitute. Only unrelated reset/promise dependencies are stubbed. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "quickjs.h"
static unsigned override_count,sub_live;
#define POCKET_MAX_PROMISES 4
#define POCKET_ERR_CLOSED "CLOSED"
typedef struct {void (*stop)(void *,const char *);} promise_ops;
typedef struct {atomic_uint request;const promise_ops *ops;void *user;} pocket_promise_t;
static pocket_promise_t promises[POCKET_MAX_PROMISES];
static void promise_release(pocket_promise_t *p){atomic_store(&p->request,0);}
#include "class_registry_under_test.inc"

/* File-local names are intentionally qualified: hub_rt and ref_rt each occur
 * in multiple C units and represent different owner addresses. A=first14,
 * C=first16; the remaining owners can be encountered by earlier applications. */
static const char *const names[]={
    "api.hub_rt","api.token_rt",
    "kasane.tx_rt","kasane.modal_rt","kasane.ref_rt","kasane.instance_rt",
    "kasane.animation_rt","kasane.template_rt","kasane.image_rt","kasane.ticket_rt",
    "kasane.scene_rt","kasane.schema_rt","ayatsuri.follow_runtime","ayatsuri.drive_runtime",
    "ayatsuri.model_runtime","ayatsuri.model_instance_runtime",
    "fs.class_rt","storage.hub_rt","net.lease_rt","net.response_rt",
    "text.session_rt","workspace.ref_rt","music.music_rt","kasane.source_cap_rt"
};
enum {LEDGER_COUNT=sizeof names/sizeof names[0],SYNTHETIC_COUNT=256};
#ifdef CLASS_REGISTRY_LEGACY
static JSRuntime *owners[SYNTHETIC_COUNT];
#define OWNER_RUNTIME(i) owners[i]
#else
static pocket_class_owner_t owners[SYNTHETIC_COUNT];
#define OWNER_RUNTIME(i) owners[i].runtime
#endif
static JSClassID ids[SYNTHETIC_COUNT];
static void register_owner(JSRuntime *rt,unsigned index){
    printf("REGISTER owner=%s index=%u\n",index<LEDGER_COUNT?names[index]:"synthetic",index);fflush(stdout);
    assert(!pocket_api_class_ready(rt,&owners[index],&ids[index]));
    assert(OWNER_RUNTIME(index)==rt&&ids[index]==0);
    ids[index]=1000+index;
    assert(pocket_api_class_ready(rt,&owners[index],&ids[index]));
    assert(ids[index]==1000+index&&OWNER_RUNTIME(index)==rt);
}
static void reset(void){
    override_count=3;sub_live=7;pocket_api_reset();assert(!override_count&&!sub_live);
    for(unsigned i=0;i<SYNTHETIC_COUNT;i++){
        assert(!OWNER_RUNTIME(i));
#ifndef CLASS_REGISTRY_LEGACY
        assert(!owners[i].next);
#endif
    }
}
#ifdef CLASS_REGISTRY_REAL_QJS
static unsigned finalized;
static void registry_finalizer(JSRuntime *rt,JSValue value){
    (void)rt;bool found=false;
    for(unsigned i=0;i<LEDGER_COUNT;i++)if(JS_GetOpaque(value,ids[i])){assert(!OWNER_RUNTIME(i)&&!owners[i].next);found=true;break;}
    assert(found);++finalized;
}
static void real_qjs(void){
    for(unsigned pass=0;pass<20;pass++){
        JSRuntime *rt=JS_NewRuntime();assert(rt);JSContext *ctx=JS_NewContext(rt);assert(ctx);JSValue objects[LEDGER_COUNT];
        for(unsigned i=0;i<LEDGER_COUNT;i++){
            assert(!pocket_api_class_ready(rt,&owners[i],&ids[i])&&!ids[i]);JS_NewClassID(rt,&ids[i]);
            JSClassDef def={.class_name=names[i],.finalizer=registry_finalizer};assert(JS_NewClass(rt,ids[i],&def)==0);
            JSClassID saved=ids[i];assert(pocket_api_class_ready(rt,&owners[i],&ids[i])&&ids[i]==saved&&JS_IsRegisteredClass(rt,ids[i]));
            objects[i]=JS_NewObjectClass(ctx,ids[i]);assert(!JS_IsException(objects[i]));JS_SetOpaque(objects[i],&ids[i]);
        }
        reset();
        for(unsigned i=0;i<LEDGER_COUNT;i++)JS_FreeValue(ctx,objects[i]);
        JS_FreeContext(ctx);JS_FreeRuntime(rt);
    }
    assert(finalized==20*LEDGER_COUNT);printf("CLASS_REGISTRY_REAL_QJS classes=24 runtimes=20 finalizers=%u resetBeforeFinalizers=1\n",finalized);
}
#endif
int main(int argc,char **argv){
    assert(argc==2);unsigned identity=1,other_identity=2;
    JSRuntime *runtime=(JSRuntime *)(void *)&identity,*other=(JSRuntime *)(void *)&other_identity;
    if(!strcmp(argv[1],"reuse")){
        for(unsigned pass=0;pass<100;pass++){
            for(unsigned i=0;i<14;i++)register_owner(runtime,i);
            /* Same-address reuse after reset is deliberate and deterministic. */
            reset();identity++;
        }
        register_owner(runtime,0);assert(!pocket_api_class_ready(other,&owners[0],&ids[0]));
        assert(!ids[0]&&OWNER_RUNTIME(0)==other);reset();
    }else if(!strcmp(argv[1],"history1-instance17")){
        for(unsigned i=0;i<14;i++)register_owner(runtime,i);
        /* Storage is a representative extra slot, not an attribution of the
         * unknown historical surface in the observed device failure. */
        register_owner(runtime,17);reset();
        for(unsigned i=0;i<16;i++){
            register_owner(runtime,i);
            if(i==14){puts("MODEL_READY_COMPLETE historical_slots=1");fflush(stdout);}
        }
        reset();
    }else if(!strcmp(argv[1],"history2-model17")){
        for(unsigned i=0;i<14;i++)register_owner(runtime,i);
        register_owner(runtime,16);register_owner(runtime,17);reset();
        for(unsigned i=0;i<16;i++){register_owner(runtime,i);}reset();
    }else if(!strcmp(argv[1],"full-ledger")){
        for(unsigned pass=0;pass<100;pass++){
            for(unsigned i=0;i<LEDGER_COUNT;i++){register_owner(runtime,i);}reset();
        }
    }else if(!strcmp(argv[1],"reset-bounded")){
        /* Each session uses only 16 slots, but the process sees 256 addresses.
         * Merely increasing a fixed process-lifetime table cannot pass this. */
        for(unsigned pass=0;pass<32;pass++){
            for(unsigned i=0;i<16;i++){register_owner(runtime,(pass%16)*16+i);}reset();
        }
    }else if(!strcmp(argv[1],"overflow")){
        for(unsigned i=0;i<SYNTHETIC_COUNT;i++)register_owner(runtime,i);
#ifdef CLASS_REGISTRY_LEGACY
        fprintf(stderr,"Expected bounded registry overflow was not rejected\n");return 2;
#else
        reset();
#endif
#ifndef CLASS_REGISTRY_LEGACY
    }else if(!strcmp(argv[1],"forget")){
        for(unsigned i=0;i<3;i++)register_owner(runtime,i);
        pocket_api_class_forget(&owners[3]);assert(!OWNER_RUNTIME(3)&&!owners[3].next);
        const unsigned order[3]={2,1,0};
        for(unsigned k=0;k<3;k++){
            unsigned i=order[k];JSClassID saved=ids[i];pocket_api_class_forget(&owners[i]);
            assert(!OWNER_RUNTIME(i)&&!owners[i].next&&ids[i]==saved);pocket_api_class_forget(&owners[i]);register_owner(runtime,i);
        }
        reset();
#endif
#ifdef CLASS_REGISTRY_REAL_QJS
    }else if(!strcmp(argv[1],"real-qjs")){real_qjs();
#endif
    }else return 2;
    printf("CLASS_REGISTRY_PASS scenario=%s ledger=%u\n",argv[1],LEDGER_COUNT);return 0;
}
