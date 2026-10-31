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
static pocket_class_registry_t class_registry;
bool pocket_api_class_ready(JSRuntime *rt,pocket_class_owner_t *owner,JSClassID *id){
    return pocket_class_registry_ready(&class_registry,rt,owner,id);
}
void pocket_api_class_forget(pocket_class_owner_t *owner){pocket_class_registry_forget(&class_registry,owner);}
static void reset_classes(void){pocket_class_registry_reset(&class_registry);}

static union {ksn_proc_gouraud_patch gouraud;ksn_proc_ellipsoid ellipse;ksn_proc_blend_ellipsoids blend;ksn_proc_clipped_ellipsoid clipped;
    struct {ksn_proc_blend_ellipsoids patch;ksn_proc_blend_region_mask mask;} region;} packets[512];
static unsigned packet_count,submit_calls,fail_submission,begin_calls;
JSValue pocket_proc_native_begin(JSContext *ctx,const char *op,pocket_proc_mark *mark){
    (void)ctx;(void)op;++begin_calls;*mark=(pocket_proc_mark){.count=(uint16_t)packet_count};return JS_UNDEFINED;
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
JSValue pocket_proc_native_blend_region(JSContext *ctx,const char *op,const ksn_proc_blend_ellipsoids *p,const ksn_proc_blend_region_mask *mask){
    int l,t,r,b;if(!ksn_proc_blend_region_bounds(p,mask,&l,&t,&r,&b))return invalid(ctx,op,"invalid blend region");
    if(fail_submission&&++submit_calls==fail_submission)return fail(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"injected submit");
    CHECK(packet_count<512);memset(&packets[packet_count],0,sizeof packets[0]);
    packets[packet_count].region.patch=*p;packets[packet_count++].region.mask=*mask;return JS_UNDEFINED;
}
JSValue pocket_proc_native_clipped_ellipsoid(JSContext *ctx,const char *op,const ksn_proc_clipped_ellipsoid *p){
    int l,t,r,b;if(!ksn_proc_clipped_ellipsoid_bounds(p,&l,&t,&r,&b))return invalid(ctx,op,"invalid clip");
    if(l>r||t>b)return JS_UNDEFINED;
    if(fail_submission&&++submit_calls==fail_submission)return fail(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"injected submit");
    CHECK(packet_count<512);memset(&packets[packet_count],0,sizeof packets[0]);packets[packet_count++].clipped=*p;return JS_UNDEFINED;
}
JSValue pocket_proc_native_gouraud(JSContext *ctx,const char *op,const ksn_proc_gouraud_patch *p){
    int l,t,r,b;unsigned cost;
    if(!ksn_proc_gouraud_bounds(p,&l,&t,&r,&b,&cost))return invalid(ctx,op,"invalid Gouraud packet");
    if(l>r||t>b)return JS_UNDEFINED;
    if(fail_submission&&++submit_calls==fail_submission)return fail(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"injected submit");
    CHECK(packet_count<512);memset(&packets[packet_count],0,sizeof packets[0]);packets[packet_count++].gouraud=*p;return JS_UNDEFINED;
}
#include "../../../main/pocket/pocket_ayatsuri_model.inc"
static uint8_t *fixture;static size_t fixture_bytes;
static JSValue static_model(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;return pocket_ayatsuri_model_from_static(c,fixture,fixture_bytes);
}
static JSValue owned_model(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;JSValue input=JS_NewArrayBufferCopy(c,fixture,fixture_bytes);
    if(JS_IsException(input))return input;
    JSValue result=js_ayatsuri_model_open(c,JS_UNDEFINED,1,&input);JS_FreeValue(c,input);return result;
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
static JSValue recorded_packet_count(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;return JS_NewUint32(c,packet_count);
}
static JSValue recorded_begin_count(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;return JS_NewUint32(c,begin_calls);
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
    JS_SetPropertyStr(ctx,global,"ownedModel",JS_NewCFunction(ctx,owned_model,"ownedModel",0));
    JS_SetPropertyStr(ctx,global,"snapshotHash",JS_NewCFunction(ctx,snapshot_hash,"snapshotHash",0));
    JS_SetPropertyStr(ctx,global,"clearPackets",JS_NewCFunction(ctx,clear_packets,"clearPackets",0));
    JS_SetPropertyStr(ctx,global,"packetCount",JS_NewCFunction(ctx,recorded_packet_count,"packetCount",0));
    JS_SetPropertyStr(ctx,global,"beginCount",JS_NewCFunction(ctx,recorded_begin_count,"beginCount",0));
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
        "globalThis.warmOpacity=new Float64Array(warmModel.count);warmActor.evaluate(warmCamera,warmLight);"
        "globalThis.warmProjection=new Float32Array(8);globalThis.warmClipIds=new Uint16Array(1);"
        "globalThis.warmClipCounts=new Uint8Array([4]);globalThis.warmClipXY=new Float32Array(16);"
        "warmClipXY.set([-500,-500,500,-500,500,500,-500,500]);";
    eval_ok(ctx,source,strlen(source));
    JSValue global=JS_GetGlobalObject(ctx),actor=JS_GetPropertyStr(ctx,global,"warmActor");
    pocket_model_actor *native=JS_GetOpaque(actor,pocket_model_instance_class);CHECK(native);
    CHECK(((uintptr_t)native->arena&7u)==4u&&((uintptr_t)native->core.arena&7u)==0u);
    JSValue eval[2]={JS_GetPropertyStr(ctx,global,"warmCamera"),JS_GetPropertyStr(ctx,global,"warmLight")};
    JSValue target[4]={JS_GetPropertyStr(ctx,global,"warmDirty"),JS_GetPropertyStr(ctx,global,"warmPose"),
        JS_GetPropertyStr(ctx,global,"warmColor"),JS_GetPropertyStr(ctx,global,"warmOpacity")};
    uint32_t cursor=0;ayatsuri_model_packet packet;ayatsuri_model_status status;
    do{status=ayatsuri_model_instance_next(&native->core,native,1,&cursor,&packet);CHECK(status==AYATSURI_MODEL_OK);}while(packet.kind!=1);
    JSValue projection[2]={JS_NewUint32(ctx,packet.node_id),JS_GetPropertyStr(ctx,global,"warmProjection")};
    JSValue clips[3]={JS_GetPropertyStr(ctx,global,"warmClipIds"),JS_GetPropertyStr(ctx,global,"warmClipCounts"),JS_GetPropertyStr(ctx,global,"warmClipXY")};
    shade_view id_view;CHECK(managed_model_buffer(ctx,clips[0],JS_TYPED_ARRAY_UINT16,true,&id_view));memcpy(id_view.data,&packet.node_id,2);
    const size_t before=allocation_calls;
    for(unsigned n=0;n<1000;n++){
        JSValue r=managed_instance_targets(ctx,actor,4,target);CHECK(!JS_IsException(r));JS_FreeValue(ctx,r);
        r=managed_instance_evaluate(ctx,actor,2,eval);CHECK(!JS_IsException(r));JS_FreeValue(ctx,r);
        packet_count=0;r=managed_instance_submit(ctx,actor,0,NULL);CHECK(!JS_IsException(r));JS_FreeValue(ctx,r);
        r=managed_instance_read_projection(ctx,actor,2,projection);CHECK(!JS_IsException(r));JS_FreeValue(ctx,r);
        packet_count=0;r=managed_instance_submit_clipped(ctx,actor,3,clips);CHECK(!JS_IsException(r));JS_FreeValue(ctx,r);
    }
    CHECK(allocation_calls==before);
    for(unsigned n=0;n<4;n++)JS_FreeValue(ctx,target[n]);
    for(unsigned n=0;n<3;n++)JS_FreeValue(ctx,clips[n]);
    JS_FreeValue(ctx,projection[0]);JS_FreeValue(ctx,projection[1]);
    JS_FreeValue(ctx,eval[0]);JS_FreeValue(ctx,eval[1]);JS_FreeValue(ctx,actor);JS_FreeValue(ctx,global);
}
static size_t foreign_buffers,opacity_skew_count,f32_alignment_rejections;
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
    uint32_t old_hash,new_hash;r=snapshot_hash(ctx,JS_UNDEFINED,0,NULL);CHECK(JS_ToUint32(ctx,&old_hash,r)==0);JS_FreeValue(ctx,r);
    uint8_t *pose_raw=malloc(count*36+2);CHECK(pose_raw);uint8_t *pose_data=pose_raw+2;
    CHECK(((uintptr_t)pose_data&3u)==2u);memset(pose_data,0,count*36);++foreign_buffers;
    JSValue pose_ab=JS_NewArrayBuffer(ctx,pose_data,count*36,foreign_buffer_free,pose_raw,false);CHECK(!JS_IsException(pose_ab));
    JSValue pose_args[3]={pose_ab,JS_NewInt32(ctx,0),JS_NewUint32(ctx,(uint32_t)count*9)};
    JSValue pose_view=JS_NewTypedArray(ctx,3,pose_args,JS_TYPED_ARRAY_FLOAT32);CHECK(!JS_IsException(pose_view));
    JSValue misaligned[4]={target[0],pose_view,target[2],target[3]};dirty.data[1]=AYATSURI_TARGET_POSITION;
    r=managed_instance_targets(ctx,actor,4,misaligned);CHECK(JS_IsException(r));JS_FreeValue(ctx,r);
    JSValue alignment_error=JS_GetException(ctx);const char *message=JS_ToCString(ctx,alignment_error);
    CHECK(message&&strstr(message,"INVALID_ARGUMENT"));JS_FreeCString(ctx,message);JS_FreeValue(ctx,alignment_error);
    packet_count=0;r=managed_instance_submit(ctx,actor,0,NULL);CHECK(!JS_IsException(r));JS_FreeValue(ctx,r);
    r=snapshot_hash(ctx,JS_UNDEFINED,0,NULL);CHECK(JS_ToUint32(ctx,&new_hash,r)==0);JS_FreeValue(ctx,r);CHECK(new_hash==old_hash);
    JS_FreeValue(ctx,pose_view);JS_FreeValue(ctx,pose_ab);CHECK(foreign_buffers==1);++f32_alignment_rejections;
    dirty.data[1]=AYATSURI_TARGET_OPACITY;
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
static void finalizer_order_contract(JSContext *ctx){
    JSValue global=JS_GetGlobalObject(ctx);
    JSValue eval[2]={JS_GetPropertyStr(ctx,global,"warmCamera"),JS_GetPropertyStr(ctx,global,"warmLight")};
    for(unsigned model_first=0;model_first<2;model_first++){
        JSValue model=owned_model(ctx,JS_UNDEFINED,0,NULL);CHECK(!JS_IsException(model));
        JSValue actor=managed_model_instance(ctx,model,0,NULL);CHECK(!JS_IsException(actor));
        pocket_model_owner *m=JS_GetOpaque(model,pocket_model_class);CHECK(m&&m->references==2&&m->instances==1);
        /* Deliberately invoke the real class cleanup in both orders and clear
         * opaque afterward to prevent a second cleanup by QuickJS. Actual JS
         * expando cycles are also collected normally in the script tests. */
        if(model_first){
            managed_model_finalizer(JS_GetRuntime(ctx),model);JS_SetOpaque(model,NULL);
            CHECK(m->closed&&!m->released&&m->references==1&&m->instances==1&&m->owned);
            JSValue result=managed_instance_evaluate(ctx,actor,2,eval);CHECK(!JS_IsException(result));JS_FreeValue(ctx,result);
            packet_count=0;result=managed_instance_submit(ctx,actor,0,NULL);CHECK(!JS_IsException(result));JS_FreeValue(ctx,result);
            managed_instance_finalizer(JS_GetRuntime(ctx),actor);JS_SetOpaque(actor,NULL);
        }else{
            managed_instance_finalizer(JS_GetRuntime(ctx),actor);JS_SetOpaque(actor,NULL);
            CHECK(!m->closed&&!m->released&&m->references==1&&!m->instances&&m->owned);
            managed_model_finalizer(JS_GetRuntime(ctx),model);JS_SetOpaque(model,NULL);
        }
        JS_FreeValue(ctx,actor);JS_FreeValue(ctx,model);
    }
    JS_FreeValue(ctx,eval[0]);JS_FreeValue(ctx,eval[1]);JS_FreeValue(ctx,global);
}
static void blend_region_contract(JSContext *ctx){
    size_t size;uint8_t *data=(uint8_t *)read_file(".cache/ayatsuri-native/blend-region.aynm",&size);
    JSValue model=pocket_ayatsuri_model_from_static(ctx,data,size);CHECK(!JS_IsException(model));
    JSValue actor=managed_model_instance(ctx,model,0,NULL);CHECK(!JS_IsException(actor));
    JSValue global=JS_GetGlobalObject(ctx),eval[2]={JS_GetPropertyStr(ctx,global,"warmCamera"),JS_GetPropertyStr(ctx,global,"warmLight")};
    JSValue r=managed_instance_evaluate(ctx,actor,2,eval);CHECK(!JS_IsException(r));JS_FreeValue(ctx,r);
    packet_count=0;r=managed_instance_submit(ctx,actor,0,NULL);CHECK(!JS_IsException(r)&&packet_count==3);JS_FreeValue(ctx,r);
    pocket_model_actor *a=JS_GetOpaque(actor,pocket_model_instance_class);uint32_t at=2;ayatsuri_model_packet p;
    CHECK(ayatsuri_model_instance_next(&a->core,a,1,&at,&p)==AYATSURI_MODEL_OK&&p.kind==3);
    CHECK(!memcmp(&packets[1].region.mask,p.storage+p.bytes-24,24));
    CHECK(!memcmp(packets[1].region.patch.lobes,p.storage+316+24*p.lobe_count,84*p.lobe_count));
    const ksn_proc_blend_region_mask saved=packets[1].region.mask;
    const char *sources[]={"new Uint16Array(0)","new Uint8Array(0)","new Float32Array(0)"};JSValue clips[3];
    for(unsigned k=0;k<3;k++){clips[k]=JS_Eval(ctx,sources[k],strlen(sources[k]),"clip-empty",JS_EVAL_TYPE_GLOBAL);CHECK(!JS_IsException(clips[k]));}
    packet_count=0;r=managed_instance_submit_clipped(ctx,actor,3,clips);CHECK(!JS_IsException(r)&&packet_count==3);JS_FreeValue(ctx,r);
    CHECK(!memcmp(&saved,&packets[1].region.mask,24));
    submit_calls=0;fail_submission=2;r=managed_instance_submit_clipped(ctx,actor,3,clips);CHECK(JS_IsException(r)&&packet_count==3);
    r=JS_GetException(ctx);JS_FreeValue(ctx,r);fail_submission=0;
    r=managed_model_dispose(ctx,model,0,NULL);CHECK(!JS_IsException(r));JS_FreeValue(ctx,r);
    packet_count=0;r=managed_instance_submit(ctx,actor,0,NULL);CHECK(!JS_IsException(r)&&packet_count==3);JS_FreeValue(ctx,r);
    r=managed_instance_dispose(ctx,actor,0,NULL);CHECK(!JS_IsException(r));JS_FreeValue(ctx,r);
    JS_FreeValue(ctx,actor);JS_FreeValue(ctx,model);for(unsigned k=0;k<3;k++)JS_FreeValue(ctx,clips[k]);
    JS_FreeValue(ctx,eval[0]);JS_FreeValue(ctx,eval[1]);JS_FreeValue(ctx,global);free(data);
    CHECK(!memcmp(&saved,&packets[1].region.mask,24));
}
/* New mesh packets use the same owned cold snapshot and actor lifetime as
 * analytic packets. The recorder proves ownership/rollback; real raster and
 * device budgets remain the separate app-host and hardware contracts. */
static void mesh_contract(JSContext *ctx){
    size_t size;char *data=read_file(".cache/ayatsuri-native/mesh.aynm",&size);
    JSValue global=JS_GetGlobalObject(ctx),input=JS_NewArrayBufferCopy(ctx,(uint8_t *)data,size);CHECK(!JS_IsException(input));
    CHECK(JS_SetPropertyStr(ctx,global,"meshSource",JS_DupValue(ctx,input))>=0);JS_FreeValue(ctx,global);
    const char *setup=
        "(()=>{const m=modelOpen(meshSource),a=m.instance(),b=m.instance(),id=m.node('patch');"
        "assert(m.count===2&&id===1&&m.stats({}).instances===2,'mesh shared actors');"
        "assert(a.nodeKind(0)===0&&a.nodeKind(id)===3&&b.nodeKind(id)===3,'mesh metadata independent actors');"
        "for(const bad of [-1,2,.5,NaN,Infinity,'1',true])rejects(()=>a.nodeKind(bad),'INVALID_ARGUMENT');"
        "rejects(()=>a.nodeKind(),'INVALID_ARGUMENT');rejects(()=>a.nodeKind(1,2),'INVALID_ARGUMENT');"
        "a.stats({set nodes(n){assert(n===2,'mesh metadata busy probe');rejects(()=>a.nodeKind(id),'BUSY');}});"
        "{const invalid=meshSource.slice(0);new Uint8Array(invalid)[0]=0;rejects(()=>modelOpen(invalid),'INVALID_ARGUMENT');}"
        "new Uint8Array(meshSource).fill(0);const c=m.instance();c.dispose();"
        "const camera=new Float32Array([0,0,300,0,0,0,1,120,67.5]),light=new Float32Array([0,0,1,1,1,1]);"
        "a.evaluate(camera,light);clearPackets();a.submit();assert(packetCount()===1,'mesh submitted');const baseline=snapshotHash();"
        "b.evaluate(camera,light);clearPackets();b.submit();assert(snapshotHash()===baseline,'mesh independent baseline');"
        "const dirty=new Uint8Array(2),poses=new Float32Array(18),colors=new Uint32Array(2),opacity=new Float64Array(2),saved=new Float32Array(9),read=new Float32Array(9);a.readPose(0,saved);"
        "for(const field of [32,64]){dirty[0]=1;poses[0]=saved[0]+7;dirty[1]=field;colors[1]=0xff0000;opacity[1]=.5;"
        "rejects(()=>a.targets(dirty,poses,colors,opacity),'INVALID_ARGUMENT');a.readPose(0,read);assert(equal(read,saved),'mesh late target rejection atomic');"
        "clearPackets();a.submit();assert(snapshotHash()===baseline,'mesh rejection preserves packet');}"
        "dirty.fill(0);dirty[0]=1;a.targets(dirty,poses,colors,opacity);a.evaluate(camera,light);clearPackets();a.submit();assert(snapshotHash()!==baseline,'mesh parent pose dependency');"
        "clearPackets();b.submit();assert(snapshotHash()===baseline,'other mesh actor unaffected');"
        "dirty.fill(0);forceSubmitFailure(1);rejects(()=>a.submit());assert(snapshotHash()===baseline,'mesh submit rollback');forceSubmitFailure(0);"
        "clearPackets();a.submit();const savedPacket=snapshotHash();"
        "a.submitClipped(new Uint16Array(0),new Uint8Array(0),new Float32Array(0));assert(packetCount()===2,'mesh in mixed clipped walk');"
        "clearPackets();a.submit();assert(snapshotHash()===savedPacket,'mesh snapshot stable');"
        "{const scene=AyatsuriNative.scene({model:m}),handle=scene.handle(id),group=scene.handle(0);"
        "assert(handle.kind===3&&group.kind===0&&scene.handle(id)===handle,'metadata cached visual handles');"
        "const state=()=>[scene.ready,scene.busy,scene.mask.join(),scene.poses.join(),scene.colors.join(),scene.opacity.join()].join('|');"
        "const materialReject=(h,value)=>{const before=state();let caught=false;try{h.material(value)}catch(e){caught=e instanceof TypeError}assert(caught&&state()===before,'material setter rejection preserves scene state');};"
        "scene.evaluate(0);const ready=state();handle.material({});group.material({});assert(state()===ready&&scene.ready,'empty unsupported material is no-op');"
        "materialReject(handle,{color:0xff00ff});materialReject(handle,{opacity:.5});materialReject(group,{color:0xffffff});materialReject(group,{opacity:.5});"
        "handle.place({position:[3,4,0]});const pending=state();materialReject(handle,{color:0xff0000,opacity:.3});"
        "handle.material({});assert(state()===pending&&!scene.ready&&scene.mask[id]===1,'rejected material retains pending pose');"
        "scene.evaluate(1);clearPackets();scene.render();assert(scene.ready&&!scene.mask.some(x=>x)&&packetCount()===1,'pending pose still evaluates and renders');"
        "const pose=new Float32Array(9);scene.instance.readPose(id,pose);assert(pose[0]===3&&pose[1]===4,'pending pose reached native');"
        "scene.dispose();rejects(()=>handle.place({position:[0,0,0]}));}"
        "globalThis.meshHarness={m,a,b,camera,light,dirty,poses,colors,opacity};})();";
    eval_ok(ctx,setup,strlen(setup));
    CHECK(packets[0].gouraud.header.vertices==4&&packets[0].gouraud.header.triangles==2&&packets[0].gouraud.header.holes==1);
    global=JS_GetGlobalObject(ctx);JSValue harness=JS_GetPropertyStr(ctx,global,"meshHarness"),actor=JS_GetPropertyStr(ctx,harness,"a");
    JSValue eval[2]={JS_GetPropertyStr(ctx,harness,"camera"),JS_GetPropertyStr(ctx,harness,"light")};
    const char *names[]={"dirty","poses","colors","opacity"};JSValue targets[4];
    for(unsigned k=0;k<4;k++)targets[k]=JS_GetPropertyStr(ctx,harness,names[k]);
    const size_t calls=allocation_calls;
    for(unsigned frame=0;frame<1000;frame++){
        JSValue value=managed_instance_targets(ctx,actor,4,targets);CHECK(!JS_IsException(value));JS_FreeValue(ctx,value);
        value=managed_instance_evaluate(ctx,actor,2,eval);CHECK(!JS_IsException(value));JS_FreeValue(ctx,value);
        packet_count=0;value=managed_instance_submit(ctx,actor,0,NULL);CHECK(!JS_IsException(value)&&packet_count==1);JS_FreeValue(ctx,value);
    }
    CHECK(allocation_calls==calls);
    for(unsigned k=0;k<4;k++)JS_FreeValue(ctx,targets[k]);
    for(unsigned k=0;k<2;k++)JS_FreeValue(ctx,eval[k]);
    JS_FreeValue(ctx,actor);JS_FreeValue(ctx,harness);JS_FreeValue(ctx,global);
    const char *cleanup="(()=>{const {m,a,b,camera,light}=meshHarness;m.dispose();rejects(()=>m.instance(),'CLOSED');"
        "assert(m.stats({}).instances===2&&!m.stats({}).released,'mesh closed cold retained');"
        "assert(a.nodeKind(0)===0&&a.nodeKind(1)===3&&b.nodeKind(1)===3,'live actor metadata after shared Model close');"
        "a.evaluate(camera,light);clearPackets();a.submit();a.dispose();assert(!m.stats({}).released,'mesh last actor protects cold');"
        "b.evaluate(camera,light);clearPackets();b.submit();const snapshot=snapshotHash();b.dispose();"
        "assert(m.stats({}).released&&m.stats({}).ownedBytes===0&&snapshotHash()===snapshot,'mesh last actor releases cold without snapshot mutation');"
        "rejects(()=>a.evaluate(camera,light),'CLOSED');rejects(()=>b.submit(),'CLOSED');rejects(()=>a.nodeKind(1),'CLOSED');rejects(()=>b.nodeKind(1),'CLOSED');delete globalThis.meshHarness;delete globalThis.meshSource;})();";
    eval_ok(ctx,cleanup,strlen(cleanup));JS_FreeValue(ctx,input);free(data);
    puts("MESH_QJS_PASS node_kind visual_material_rejection pending_pose copied_source shared_actors targets_atomic snapshot rollback clipped_walk retained_close warm1000_allocations0");
}
static void mesh_oom_contract(void){
    size_t bytes;uint8_t *data=(uint8_t *)read_file(".cache/ayatsuri-native/mesh.aynm",&bytes);size_t failures=0;
    for(unsigned site=1;site<=64;site++){
        JSRuntime *rt=JS_NewRuntime2(&allocator,NULL);CHECK(rt);JSContext *ctx=JS_NewContext(rt);CHECK(ctx);
        JSValue input=JS_NewArrayBufferCopy(ctx,data,bytes);CHECK(!JS_IsException(input));
        fail_at=allocation_calls+site;const size_t injected=injected_ooms;
        JSValue model=js_ayatsuri_model_open(ctx,JS_UNDEFINED,1,&input),actor=JS_UNDEFINED;
        if(!JS_IsException(model))actor=managed_model_instance(ctx,model,0,NULL);
        if(JS_HasException(ctx)){JSValue error=JS_GetException(ctx);JS_FreeValue(ctx,error);}
        fail_at=0;failures+=injected_ooms!=injected;JS_FreeValue(ctx,actor);JS_FreeValue(ctx,model);
        model=js_ayatsuri_model_open(ctx,JS_UNDEFINED,1,&input);CHECK(!JS_IsException(model));
        actor=managed_model_instance(ctx,model,0,NULL);CHECK(!JS_IsException(actor));
        JS_FreeValue(ctx,actor);JS_FreeValue(ctx,model);JS_FreeValue(ctx,input);reset_classes();JS_FreeContext(ctx);JS_FreeRuntime(rt);CHECK(retained==0);
    }
    CHECK(failures>0);free(data);printf("MESH_QJS_OOM_PASS positions64 injected_positions=%zu retained=0\n",failures);
}
/* Deliberately exceed the old process-global owner limit before exercising
 * the REAL Model/Instance constructors. These unrelated QuickJS classes are
 * a registry stress fixture, not substitutes for physical device surfaces. */
static pocket_class_owner_t history_owners[24];
static JSClassID history_ids[24];
static void seed_class_history(JSContext *ctx){
    JSRuntime *rt=JS_GetRuntime(ctx);
    const JSClassDef definition={.class_name="RegistryHistory"};
    for(unsigned i=0;i<24;i++){
        CHECK(!pocket_api_class_ready(rt,&history_owners[i],&history_ids[i]));
        JS_NewClassID(rt,&history_ids[i]);CHECK(JS_NewClass(rt,history_ids[i],&definition)==0);
        JSClassID saved=history_ids[i];
        CHECK(pocket_api_class_ready(rt,&history_owners[i],&history_ids[i])&&history_ids[i]==saved);
    }
}
int main(int argc,char **argv){
    CHECK(argc>=2);fixture=(uint8_t *)read_file(argv[1],&fixture_bytes);
    size_t source_bytes;char *source=read_file(argc>2?argv[2]:"tools/ayatsuri/model/test_model_qjs.js",&source_bytes);
    size_t wrapper_bytes;char *wrapper=read_file("apps/lib/ayatsuri_model.js",&wrapper_bytes);
    for(unsigned run=0;run<4;run++){
        JSRuntime *rt=JS_NewRuntime2(&allocator,NULL);CHECK(rt);JSContext *ctx=JS_NewContext(rt);CHECK(ctx);
        seed_class_history(ctx);
        /* Surface sets can differ between sessions; reserve unrelated class
         * ids so accidentally reused process-global ids cannot pass by luck. */
        if(run&1){JSClassID id=0;JS_NewClassID(rt,&id);const JSClassDef d={.class_name="OtherSurface"};CHECK(JS_NewClass(rt,id,&d)==0);}
        ayatsuri_model model;CHECK(ayatsuri_model_open(fixture,fixture_bytes,&model)==AYATSURI_MODEL_OK);
        const size_t before_skew=skew_count;skew_bytes=ayatsuri_model_instance_bytes(&model)+7u;
        packet_count=submit_calls=fail_submission=begin_calls=0;install(ctx);eval_ok(ctx,wrapper,wrapper_bytes);
        eval_ok(ctx,source,source_bytes);warm_contract(ctx);unaligned_opacity_contract(ctx);finalizer_order_contract(ctx);blend_region_contract(ctx);mesh_contract(ctx);JS_RunGC(rt);
        CHECK(skew_count>before_skew);skew_bytes=0;
        reset_classes();JS_FreeContext(ctx);JS_FreeRuntime(rt);CHECK(retained==0);
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
        JS_FreeValue(ctx,a);JS_FreeValue(ctx,m);reset_classes();JS_FreeContext(ctx);JS_FreeRuntime(rt);CHECK(retained==0);
    }
    mesh_oom_contract();
    free(wrapper);free(source);free(fixture);printf("PASS native Model/Instance: real QuickJS, 4 runtimes after 24 preexisting class owners, explicit cold release/both finalizer orders, actual JS wrapper visibility/dirty/atomicity, projection/clipped batch prevalidation, two independent instances, snapshot/rollback, arena 4-mod-8 skew=%zu, opacity 4-mod-8=%zu, F32 2-mod-4 rejected=%zu, 1000 warm targets/evaluate/submit/readProjection/submitClipped zero allocations, 192 OOM positions/%zu injected, retained=0\n",skew_count,opacity_skew_count,f32_alignment_rejections,injected_ooms);return 0;
}
