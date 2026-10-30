/* Real QuickJS ownership tests. Renderer stubs copy the exact native packets;
 * app-host integration separately exercises pocket_proc's actual renderer.
 * Stub rollback proves packet removal, not that a real budget-poisoned native
 * candidate can commit: that turn must begin again under existing semantics. */
#include "quickjs.h"
#include "pocket_api.h"
#include "pocket_proc.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)
typedef struct {uint8_t *data;size_t bytes;} shade_view;
static JSValue fail(JSContext *c,const char *op,const char *code,const char *s){return JS_ThrowTypeError(c,"%s %s: %s",code,op,s);}
static JSValue invalid(JSContext *c,const char *op,const char *s){return fail(c,op,POCKET_ERR_INVALID_ARGUMENT,s);}
static bool number(JSContext *c,JSValueConst v,double *out){return JS_IsNumber(v)&&JS_ToFloat64(c,out,v)==0;}
static bool whole(double n,double a,double b){return n>=a&&n<=b&&n==floor(n);}
static JSRuntime **class_owners[8];static unsigned class_owner_count;
bool pocket_api_class_ready(JSRuntime *rt,JSRuntime **owner,JSClassID *id){
    bool found=false;for(unsigned n=0;n<class_owner_count;n++)if(class_owners[n]==owner)found=true;
    if(!found){CHECK(class_owner_count<8);class_owners[class_owner_count++]=owner;}
    if(*owner==rt)return true;
    *owner=rt;*id=0;return false;
}
static void reset_classes(void){for(unsigned n=0;n<class_owner_count;n++)*class_owners[n]=NULL;}
static union {ksn_proc_ellipsoid ellipse;ksn_proc_blend_ellipsoids blend;} packets[512];
static unsigned packet_count,submit_calls,fail_submission;
JSValue pocket_proc_native_begin(JSContext *ctx,const char *op,pocket_proc_mark *mark){
    (void)ctx;(void)op;*mark=(pocket_proc_mark){.count=(uint16_t)packet_count};return JS_UNDEFINED;
}
void pocket_proc_native_rollback(const pocket_proc_mark *mark){packet_count=mark->count;}
JSValue pocket_proc_native_ellipsoid(JSContext *ctx,const char *op,const ksn_proc_ellipsoid *p){
    if(fail_submission&&++submit_calls==fail_submission)return fail(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"injected submit");
    CHECK(packet_count<512);memset(&packets[packet_count],0,sizeof packets[0]);packets[packet_count++].ellipse=*p;return JS_UNDEFINED;
}
JSValue pocket_proc_native_blend_ellipsoids(JSContext *ctx,const char *op,const ksn_proc_blend_ellipsoids *p){
    if(fail_submission&&++submit_calls==fail_submission)return fail(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"injected submit");
    CHECK(packet_count<512);memset(&packets[packet_count],0,sizeof packets[0]);packets[packet_count++].blend=*p;return JS_UNDEFINED;
}
#include "../../../main/pocket/pocket_ayatsuri_model.inc"
static uint8_t *fixture;static size_t fixture_bytes;
static JSValue static_model(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;return pocket_ayatsuri_model_from_static(c,fixture,fixture_bytes);
}
static JSValue snapshot_hash(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;uint32_t hash=2166136261u;
    const uint8_t *p=(const uint8_t *)packets;
    for(size_t n=0;n<packet_count*sizeof packets[0];n++)hash=(hash^p[n])*16777619u;
    return JS_NewUint32(c,hash);
}
static JSValue clear_packets(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)c;(void)self;(void)argc;(void)argv;packet_count=0;return JS_UNDEFINED;
}
static JSValue force_failure(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;CHECK(argc==1);CHECK(JS_ToUint32(c,&fail_submission,argv[0])==0);submit_calls=0;return JS_UNDEFINED;
}
typedef struct {size_t n;void *base;size_t pad[2];} allocation;
static size_t retained,allocation_calls,fail_at,injected_ooms,skew_bytes,skew_count;
static allocation tracked_header(const void *data){
    allocation h;memcpy(&h,(const uint8_t *)data-sizeof h,sizeof h);return h;
}
static void *tracked_malloc(void *opaque,size_t n){
    (void)opaque;if(!n)return NULL;++allocation_calls;if(fail_at&&allocation_calls==fail_at){++injected_ooms;return NULL;}
    const size_t skew=skew_bytes&&n==skew_bytes?4u:0u;
    if(n>SIZE_MAX-sizeof(allocation)-skew)return NULL;
    void *base=malloc(sizeof(allocation)+skew+n);if(!base)return NULL;
    const allocation h={.n=n,.base=base};uint8_t *data=(uint8_t *)base+sizeof h+skew;
    /* Header itself may be 4 mod 8. memcpy avoids unaligned size_t/pointer UB
     * while exercising only the arena's weaker device alignment. */
    memcpy(data-sizeof h,&h,sizeof h);retained+=n;
    if(skew){CHECK(((uintptr_t)data&7u)==4u);++skew_count;}
    return data;
}
static void tracked_free(void *opaque,void *data){(void)opaque;if(data){const allocation h=tracked_header(data);retained-=h.n;free(h.base);}}
static void *tracked_calloc(void *opaque,size_t n,size_t bytes){if(bytes&&n>SIZE_MAX/bytes)return NULL;void *p=tracked_malloc(opaque,n*bytes);if(p)memset(p,0,n*bytes);return p;}
static void *tracked_realloc(void *opaque,void *data,size_t n){
    if(!data)return tracked_malloc(opaque,n);
    if(!n){tracked_free(opaque,data);return NULL;}
    ++allocation_calls;if(fail_at&&allocation_calls==fail_at){++injected_ooms;return NULL;}
    const allocation old=tracked_header(data);
    const size_t skew=(size_t)((uint8_t *)data-(uint8_t *)old.base-sizeof old);
    if(n>SIZE_MAX-sizeof(allocation)-skew)return NULL;
    void *base=realloc(old.base,sizeof(allocation)+skew+n);if(!base)return NULL;
    const allocation h={.n=n,.base=base};uint8_t *result=(uint8_t *)base+sizeof h+skew;
    memcpy(result-sizeof h,&h,sizeof h);retained=retained-old.n+n;return result;
}
static size_t tracked_usable(const void *data){return data?tracked_header(data).n:0;}
static const JSMallocFunctions allocator={tracked_calloc,tracked_malloc,tracked_free,tracked_realloc,tracked_usable};
static char *read_file(const char *path,size_t *bytes){
    FILE *f=fopen(path,"rb");CHECK(f);CHECK(fseek(f,0,SEEK_END)==0);long n=ftell(f);CHECK(n>0);rewind(f);
    char *p=malloc((size_t)n+1);CHECK(p);CHECK(fread(p,1,(size_t)n,f)==(size_t)n);fclose(f);p[n]=0;*bytes=(size_t)n;return p;
}
static void eval_ok(JSContext *c,const char *source,size_t bytes){
    JSValue result=JS_Eval(c,source,bytes,"native-model-test.js",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(result)){JSValue error=JS_GetException(c);const char *message=JS_ToCString(c,error);fprintf(stderr,"QuickJS: %s\n",message?message:"exception");JS_FreeCString(c,message);JS_FreeValue(c,error);exit(1);}JS_FreeValue(c,result);
}
static void install(JSContext *ctx){
    JSValue global=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,global,"modelOpen",JS_NewCFunction(ctx,js_ayatsuri_model_open,"modelOpen",1));
    JS_SetPropertyStr(ctx,global,"staticModel",JS_NewCFunction(ctx,static_model,"staticModel",0));
    JS_SetPropertyStr(ctx,global,"snapshotHash",JS_NewCFunction(ctx,snapshot_hash,"snapshotHash",0));
    JS_SetPropertyStr(ctx,global,"clearPackets",JS_NewCFunction(ctx,clear_packets,"clearPackets",0));
    JS_SetPropertyStr(ctx,global,"forceSubmitFailure",JS_NewCFunction(ctx,force_failure,"forceSubmitFailure",1));
    JS_SetPropertyStr(ctx,global,"sourceBuffer",JS_NewArrayBufferCopy(ctx,fixture,fixture_bytes));
    ayatsuri_model model;CHECK(ayatsuri_model_open(fixture,fixture_bytes,&model)==AYATSURI_MODEL_OK);
    const uint8_t *binding=fixture+model.section[5].offset;uint16_t len;uint32_t offset;
    memcpy(&len,binding+2,2);memcpy(&offset,binding+4,4);
    JS_SetPropertyStr(ctx,global,"firstName",JS_NewStringLen(ctx,(const char *)fixture+model.section[6].offset+offset,len));
    JS_FreeValue(ctx,global);
}
static void warm_contract(JSContext *ctx){
    const char *source="globalThis.warmModel=staticModel();globalThis.warmActor=warmModel.instance();"
        "globalThis.warmCamera=new Float32Array([0,0,300,0,0,0,1,120,67.5]);"
        "globalThis.warmLight=new Float32Array([0,0,1,1,1,1]);"
        "globalThis.warmDirty=new Uint8Array(warmModel.count);"
        "globalThis.warmPose=new Float32Array(warmModel.count*9);"
        "globalThis.warmColor=new Uint32Array(warmModel.count);"
        "globalThis.warmOpacity=new Float64Array(warmModel.count);warmActor.evaluate(warmCamera,warmLight);";
    eval_ok(ctx,source,strlen(source));
    JSValue global=JS_GetGlobalObject(ctx),actor=JS_GetPropertyStr(ctx,global,"warmActor");
    pocket_model_actor *native=JS_GetOpaque(actor,pocket_model_instance_class);CHECK(native);
    CHECK(((uintptr_t)native->arena&7u)==4u&&((uintptr_t)native->core.arena&7u)==0u);
    JSValue eval[2]={JS_GetPropertyStr(ctx,global,"warmCamera"),JS_GetPropertyStr(ctx,global,"warmLight")};
    JSValue target[4]={JS_GetPropertyStr(ctx,global,"warmDirty"),JS_GetPropertyStr(ctx,global,"warmPose"),
        JS_GetPropertyStr(ctx,global,"warmColor"),JS_GetPropertyStr(ctx,global,"warmOpacity")};
    const size_t before=allocation_calls;
    for(unsigned n=0;n<1000;n++){
        JSValue r=managed_instance_targets(ctx,actor,4,target);CHECK(!JS_IsException(r));JS_FreeValue(ctx,r);
        r=managed_instance_evaluate(ctx,actor,2,eval);CHECK(!JS_IsException(r));JS_FreeValue(ctx,r);
        packet_count=0;r=managed_instance_submit(ctx,actor,0,NULL);CHECK(!JS_IsException(r));JS_FreeValue(ctx,r);
    }
    CHECK(allocation_calls==before);
    for(unsigned n=0;n<4;n++)JS_FreeValue(ctx,target[n]);
    JS_FreeValue(ctx,eval[0]);JS_FreeValue(ctx,eval[1]);JS_FreeValue(ctx,actor);JS_FreeValue(ctx,global);
}
static size_t foreign_buffers,opacity_skew_count;
static void foreign_buffer_free(JSRuntime *rt,void *opaque,void *data){
    (void)rt;(void)data;CHECK(foreign_buffers);--foreign_buffers;free(opaque);
}
static void unaligned_opacity_contract(JSContext *ctx){
    JSValue global=JS_GetGlobalObject(ctx),actor=JS_GetPropertyStr(ctx,global,"warmActor");
    pocket_model_actor *native=JS_GetOpaque(actor,pocket_model_instance_class);CHECK(native);
    const size_t count=native->core.model->node_count;CHECK(count>2);
    uint8_t *raw=malloc(count*8+4);CHECK(raw);uint8_t *data=raw+4;
    CHECK(((uintptr_t)data&7u)==4u);
    const double alpha=.5;for(size_t n=0;n<count;n++)memcpy(data+n*8,&alpha,8);
    ++foreign_buffers;
    JSValue ab=JS_NewArrayBuffer(ctx,data,count*8,foreign_buffer_free,raw,false);CHECK(!JS_IsException(ab));
    /* Construct only a view. JS never reads/writes its unaligned float data;
     * selected values are initialized/changed here with memcpy. */
    JSValue view_args[3]={ab,JS_NewInt32(ctx,0),JS_NewUint32(ctx,(uint32_t)count)};
    JSValue opacity=JS_NewTypedArray(ctx,3,view_args,JS_TYPED_ARRAY_FLOAT64);CHECK(!JS_IsException(opacity));
    JSValue target[4]={JS_GetPropertyStr(ctx,global,"warmDirty"),JS_GetPropertyStr(ctx,global,"warmPose"),
        JS_GetPropertyStr(ctx,global,"warmColor"),opacity};
    shade_view dirty;CHECK(managed_model_buffer(ctx,target[0],JS_TYPED_ARRAY_UINT8,true,&dirty));
    memset(dirty.data,0,count);dirty.data[1]=AYATSURI_TARGET_OPACITY;
    JSValue r=managed_instance_targets(ctx,actor,4,target);CHECK(!JS_IsException(r));JS_FreeValue(ctx,r);
    JSValue eval[2]={JS_GetPropertyStr(ctx,global,"warmCamera"),JS_GetPropertyStr(ctx,global,"warmLight")};
    r=managed_instance_evaluate(ctx,actor,2,eval);CHECK(!JS_IsException(r));JS_FreeValue(ctx,r);
    uint32_t cursor=0;ayatsuri_model_packet packet;
    CHECK(ayatsuri_model_instance_next(&native->core,native,1,&cursor,&packet)==AYATSURI_MODEL_OK&&packet.node_id==1);
    uint16_t quantized;memcpy(&quantized,packet.storage+(packet.kind==1?236:254),2);CHECK(quantized==128);
    packet_count=0;r=managed_instance_submit(ctx,actor,0,NULL);CHECK(!JS_IsException(r));JS_FreeValue(ctx,r);
    const double changed=.25,invalid_alpha=NAN;
    memcpy(data+8,&changed,8);memcpy(data+(count-1)*8,&invalid_alpha,8);dirty.data[count-1]=AYATSURI_TARGET_OPACITY;
    r=managed_instance_targets(ctx,actor,4,target);CHECK(JS_IsException(r));JS_FreeValue(ctx,r);
    JSValue error=JS_GetException(ctx);JS_FreeValue(ctx,error);
    cursor=0;CHECK(ayatsuri_model_instance_next(&native->core,native,1,&cursor,&packet)==AYATSURI_MODEL_OK);
    memcpy(&quantized,packet.storage+(packet.kind==1?236:254),2);CHECK(quantized==128);
    memset(dirty.data,0,count);
    for(unsigned n=0;n<4;n++)JS_FreeValue(ctx,target[n]);
    JS_FreeValue(ctx,ab);JS_FreeValue(ctx,eval[0]);JS_FreeValue(ctx,eval[1]);
    JS_FreeValue(ctx,actor);JS_FreeValue(ctx,global);CHECK(foreign_buffers==0);++opacity_skew_count;
}
int main(int argc,char **argv){
    CHECK(argc>=2);fixture=(uint8_t *)read_file(argv[1],&fixture_bytes);
    size_t source_bytes;char *source=read_file(argc>2?argv[2]:"tools/ayatsuri/model/test_model_qjs.js",&source_bytes);
    for(unsigned run=0;run<4;run++){
        JSRuntime *rt=JS_NewRuntime2(&allocator,NULL);CHECK(rt);JSContext *ctx=JS_NewContext(rt);CHECK(ctx);
        /* Surface sets can differ between sessions; reserve unrelated class
         * ids so accidentally reused process-global ids cannot pass by luck. */
        if(run&1){JSClassID id=0;JS_NewClassID(rt,&id);const JSClassDef d={.class_name="OtherSurface"};CHECK(JS_NewClass(rt,id,&d)==0);}
        ayatsuri_model model;CHECK(ayatsuri_model_open(fixture,fixture_bytes,&model)==AYATSURI_MODEL_OK);
        const size_t before_skew=skew_count;skew_bytes=ayatsuri_model_instance_bytes(&model)+7u;
        packet_count=submit_calls=fail_submission=0;install(ctx);eval_ok(ctx,source,source_bytes);warm_contract(ctx);unaligned_opacity_contract(ctx);JS_RunGC(rt);
        CHECK(skew_count>before_skew);skew_bytes=0;
        JS_FreeContext(ctx);reset_classes();JS_FreeRuntime(rt);CHECK(retained==0);
    }
    /* Force every allocation site in class/model/arena construction once;
     * after restoring allocation, the same runtime must recover. */
    for(unsigned trial=0;trial<192;trial++){
        JSRuntime *rt=JS_NewRuntime2(&allocator,NULL);CHECK(rt);JSContext *ctx=JS_NewContext(rt);CHECK(ctx);
        JSValue input=JS_NewArrayBufferCopy(ctx,fixture,fixture_bytes);CHECK(!JS_IsException(input));
        allocation_calls=0;fail_at=trial%96+1;
        JSValue m=trial<96?static_model(ctx,JS_UNDEFINED,0,NULL):js_ayatsuri_model_open(ctx,JS_UNDEFINED,1,&input);
        JSValue a=JS_UNDEFINED;
        if(!JS_IsException(m))a=managed_model_instance(ctx,m,0,NULL);
        if(JS_HasException(ctx)){JSValue error=JS_GetException(ctx);JS_FreeValue(ctx,error);}
        fail_at=0;JS_FreeValue(ctx,a);JS_FreeValue(ctx,m);JS_FreeValue(ctx,input);
        m=static_model(ctx,JS_UNDEFINED,0,NULL);CHECK(!JS_IsException(m));
        a=managed_model_instance(ctx,m,0,NULL);CHECK(!JS_IsException(a));
        JS_FreeValue(ctx,a);JS_FreeValue(ctx,m);JS_FreeContext(ctx);reset_classes();JS_FreeRuntime(rt);CHECK(retained==0);
    }
    free(source);free(fixture);printf("PASS native Model/Instance: real QuickJS, 4 runtimes, two independent instances, snapshot/rollback, arena 4-mod-8 skew=%zu, opacity 4-mod-8=%zu, 1000 warm targets/evaluate/submit zero allocations, 192 OOM positions/%zu injected, retained=0\n",skew_count,opacity_skew_count,injected_ooms);return 0;
}
