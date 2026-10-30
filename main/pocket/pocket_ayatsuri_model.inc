/* Immutable cold model + independently owned mutable instances. Include after
 * shade_buffer/fail helpers. No JS backing pointer survives a synchronous call.
 * All native storage is charged to the ordinary guest allocator. */
#include "ayatsuri/ayatsuri_model.h"

typedef struct {
    ayatsuri_model model;
    uint8_t *owned;
    bool closed,busy;
} pocket_model_owner;
typedef struct {
    ayatsuri_model_instance core;
    JSValue owner;
    void *arena;
    bool closed,busy;
} pocket_model_actor;
static JSClassID pocket_model_class,pocket_model_instance_class;
static JSRuntime *pocket_model_runtime,*pocket_model_instance_runtime;

static bool managed_model_buffer(JSContext *ctx,JSValueConst value,JSTypedArrayEnum type,
                                 bool writable,shade_view *out){
    if(JS_GetTypedArrayType(value)!=(int)type)return false;
    size_t offset=0,bytes=0,per=0,size=0;
    JSValue ab=JS_GetTypedArrayBuffer(ctx,value,&offset,&bytes,&per);
    if(JS_IsException(ab))return false;
    /* A concurrently writable SharedArrayBuffer cannot be synchronously
     * validated and borrowed atomically. Reject it for both inputs/outputs. */
    const bool ordinary=JS_IsArrayBuffer(ab);
    uint8_t *data=ordinary?JS_GetArrayBuffer(ctx,&size,ab):NULL;
    const bool ok=data&&offset<=size&&bytes<=size-offset&&
        (!writable||JS_IsImmutableArrayBuffer(ab)<=0);
    if(ok)*out=(shade_view){data+offset,bytes};
    JS_FreeValue(ctx,ab);return ok;
}

static JSValue managed_model_error(JSContext *ctx,const char *op,ayatsuri_model_status s){
    if(JS_HasException(ctx))return JS_EXCEPTION;
    if(s==AYATSURI_MODEL_RANGE)return fail(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"model resource or numeric range");
    if(s==AYATSURI_MODEL_BUSY)return fail(ctx,op,POCKET_ERR_BUSY,"busy model instance");
    if(s==AYATSURI_MODEL_STATE)return fail(ctx,op,POCKET_ERR_CLOSED,"model instance not available or not evaluated");
    if(s==AYATSURI_MODEL_ENDIAN)return fail(ctx,op,POCKET_ERR_UNSUPPORTED,"little-endian IEEE model target required");
    return invalid(ctx,op,"invalid native model argument");
}
static pocket_model_owner *managed_model_enter(JSContext *ctx,JSValueConst self,const char *op,bool allow_closed){
    pocket_model_owner *m=JS_GetOpaque2(ctx,self,pocket_model_class);
    if(!m)return NULL;
    if(m->busy){fail(ctx,op,POCKET_ERR_BUSY,"busy model");return NULL;}
    if(m->closed&&!allow_closed){fail(ctx,op,POCKET_ERR_CLOSED,"model disposed");return NULL;}
    m->busy=true;return m;
}
static pocket_model_actor *managed_instance_enter(JSContext *ctx,JSValueConst self,const char *op,bool allow_closed){
    pocket_model_actor *a=JS_GetOpaque2(ctx,self,pocket_model_instance_class);
    if(!a)return NULL;
    if(a->busy){fail(ctx,op,POCKET_ERR_BUSY,"busy model instance");return NULL;}
    if(a->closed&&!allow_closed){fail(ctx,op,POCKET_ERR_CLOSED,"model instance disposed");return NULL;}
    a->busy=true;return a;
}
static void managed_instance_release(JSRuntime *rt,pocket_model_actor *a){
    if(!a->closed){
        (void)ayatsuri_model_instance_dispose(&a->core,a,1);
        js_free_rt(rt,a->arena);a->arena=NULL;
        JS_FreeValueRT(rt,a->owner);a->owner=JS_UNDEFINED;a->closed=true;
    }
}
static void managed_model_finalizer(JSRuntime *rt,JSValue object){
    pocket_model_owner *m=JS_GetOpaque(object,pocket_model_class);
    if(m){js_free_rt(rt,m->owned);js_free_rt(rt,m);}
}
static void managed_instance_finalizer(JSRuntime *rt,JSValue object){
    pocket_model_actor *a=JS_GetOpaque(object,pocket_model_instance_class);
    if(a){managed_instance_release(rt,a);js_free_rt(rt,a);}
}
static void managed_instance_mark(JSRuntime *rt,JSValueConst object,JS_MarkFunc *mark){
    pocket_model_actor *a=JS_GetOpaque(object,pocket_model_instance_class);
    if(a)JS_MarkValue(rt,a->owner,mark);
}
static JSValue managed_model_count(JSContext *ctx,JSValueConst self){
    const char *op="ayatsuri.model.count";pocket_model_owner *m=managed_model_enter(ctx,self,op,false);
    if(!m)return JS_EXCEPTION;
    JSValue result=JS_NewUint32(ctx,m->model.node_count);m->busy=false;return result;
}
static JSValue managed_model_node(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    const char *op="ayatsuri.model.node";pocket_model_owner *m=managed_model_enter(ctx,self,op,false);
    if(!m)return JS_EXCEPTION;
    uint16_t node=0;ayatsuri_model_status status=AYATSURI_MODEL_INVALID;
    if(argc==1&&JS_IsString(argv[0])){
        size_t bytes=0;const char *name=JS_ToCStringLen(ctx,&bytes,argv[0]);
        if(name){status=ayatsuri_model_find_node(&m->model,name,bytes,&node);JS_FreeCString(ctx,name);}
    }
    JSValue result=status==AYATSURI_MODEL_OK?JS_NewUint32(ctx,node):managed_model_error(ctx,op,status);
    m->busy=false;return result;
}
static JSValue managed_model_dispose(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)argv;const char *op="ayatsuri.model.dispose";pocket_model_owner *m=managed_model_enter(ctx,self,op,true);
    if(!m)return JS_EXCEPTION;
    if(argc){m->busy=false;return invalid(ctx,op,"dispose takes no arguments");}
    /* Existing instances retain this owner and its immutable bytes. */
    m->closed=true;m->busy=false;return JS_UNDEFINED;
}
static JSValue managed_instance_dispose(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)argv;const char *op="ayatsuri.instance.dispose";pocket_model_actor *a=managed_instance_enter(ctx,self,op,true);
    if(!a)return JS_EXCEPTION;
    if(argc){a->busy=false;return invalid(ctx,op,"dispose takes no arguments");}
    managed_instance_release(JS_GetRuntime(ctx),a);a->busy=false;return JS_UNDEFINED;
}
static JSValue managed_instance_targets(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    const char *op="ayatsuri.instance.targets";pocket_model_actor *a=managed_instance_enter(ctx,self,op,false);
    if(!a)return JS_EXCEPTION;
    const size_t n=a->core.model->node_count;shade_view v[4];bool valid=argc==4;
    const JSTypedArrayEnum types[4]={JS_TYPED_ARRAY_UINT8,JS_TYPED_ARRAY_FLOAT32,JS_TYPED_ARRAY_UINT32,JS_TYPED_ARRAY_FLOAT64};
    const size_t bytes[4]={n,n*36,n*4,n*8};
    for(unsigned i=0;valid&&i<4;++i)valid=managed_model_buffer(ctx,argv[i],types[i],false,&v[i])&&v[i].bytes==bytes[i];
    ayatsuri_model_status status=AYATSURI_MODEL_INVALID;
    if(valid){
        const ayatsuri_model_targets batch={v[0].data,(const float *)v[1].data,
            (const uint32_t *)v[2].data,v[3].data,n};
        status=ayatsuri_model_instance_targets(&a->core,a,1,&batch);
    }
    a->busy=false;return status==AYATSURI_MODEL_OK?JS_UNDEFINED:managed_model_error(ctx,op,status);
}
static JSValue managed_instance_evaluate(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    const char *op="ayatsuri.instance.evaluate";pocket_model_actor *a=managed_instance_enter(ctx,self,op,false);
    if(!a)return JS_EXCEPTION;
    shade_view camera,light;ayatsuri_model_status status=AYATSURI_MODEL_INVALID;
    if(argc==2&&managed_model_buffer(ctx,argv[0],JS_TYPED_ARRAY_FLOAT32,false,&camera)&&camera.bytes==36&&
       managed_model_buffer(ctx,argv[1],JS_TYPED_ARRAY_FLOAT32,false,&light)&&light.bytes==24){
        float c[9],l[6];memcpy(c,camera.data,sizeof c);memcpy(l,light.data,sizeof l);
        status=ayatsuri_model_instance_evaluate(&a->core,a,1,c,l);
    }
    a->busy=false;return status==AYATSURI_MODEL_OK?JS_UNDEFINED:managed_model_error(ctx,op,status);
}
static JSValue managed_instance_submit(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)argv;const char *op="ayatsuri.instance.submit";pocket_model_actor *a=managed_instance_enter(ctx,self,op,false);
    if(!a)return JS_EXCEPTION;
    if(argc){a->busy=false;return invalid(ctx,op,"submit takes no arguments");}
    uint32_t cursor=0;ayatsuri_model_packet packet;
    ayatsuri_model_status status=ayatsuri_model_instance_next(&a->core,a,1,&cursor,&packet);
    if(status!=AYATSURI_MODEL_OK&&status!=AYATSURI_MODEL_DONE){a->busy=false;return managed_model_error(ctx,op,status);}
    pocket_proc_mark mark;JSValue result=pocket_proc_native_begin(ctx,op,&mark);
    if(JS_IsException(result)){a->busy=false;return result;}
    while(status==AYATSURI_MODEL_OK){
        if(packet.kind==1){
            ksn_proc_ellipsoid p={0};
            memcpy(p.screen,packet.storage+96,32);memcpy(p.normal,packet.storage+128,36);
            memcpy(p.material,packet.storage+164,32);memcpy(p.texture,packet.storage+196,36);
            memcpy(p.surface,packet.storage+232,10);memcpy(p.light,packet.lighting,36);
            result=pocket_proc_native_ellipsoid(ctx,op,&p);
        }else if(packet.kind==2){
            ksn_proc_blend_ellipsoids p={0};p.header.count=packet.lobe_count;
            /* Blend storage layout is the same pure-C utility work packet. */
            memcpy(p.header.light,packet.lighting,36);
            memcpy(p.header.surface,packet.storage+252,6);
            memcpy(p.header.material,packet.storage+264,44);memcpy(p.header.controls,packet.storage+308,8);
            memcpy(p.lobes,packet.storage+316+24*packet.lobe_count,84*packet.lobe_count);
            result=pocket_proc_native_blend_ellipsoids(ctx,op,&p);
        }else result=invalid(ctx,op,"invalid model packet kind");
        if(JS_IsException(result))break;
        status=ayatsuri_model_instance_next(&a->core,a,1,&cursor,&packet);
    }
    if(!JS_IsException(result)&&status!=AYATSURI_MODEL_DONE)result=managed_model_error(ctx,op,status);
    if(JS_IsException(result))pocket_proc_native_rollback(&mark);
    a->busy=false;return result;
}
static JSValue managed_instance_stats(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    const char *op="ayatsuri.instance.stats";pocket_model_actor *a=managed_instance_enter(ctx,self,op,false);
    if(!a)return JS_EXCEPTION;
    bool valid=argc==1&&JS_IsObject(argv[0]);
    /* Output setters can run arbitrary JS; busy remains set throughout. No
     * instance mutation occurs and no backing memory is borrowed here. */
    if(valid)valid=JS_SetPropertyStr(ctx,argv[0],"nodes",JS_NewUint32(ctx,a->core.model->node_count))>=0;
    if(valid)valid=JS_SetPropertyStr(ctx,argv[0],"packets",JS_NewUint32(ctx,a->core.model->packet_count))>=0;
    if(valid)valid=JS_SetPropertyStr(ctx,argv[0],"arenaBytes",JS_NewFloat64(ctx,(double)a->core.arena_bytes))>=0;
    if(valid)valid=JS_SetPropertyStr(ctx,argv[0],"mutationEpoch",JS_NewUint32(ctx,a->core.mutation_epoch))>=0;
    if(valid)valid=JS_SetPropertyStr(ctx,argv[0],"evaluated",JS_NewBool(ctx,a->core.evaluated))>=0;
    const ayatsuri_model_stats s=a->core.stats;
    if(valid)valid=JS_SetPropertyStr(ctx,argv[0],"evalCalls",JS_NewUint32(ctx,s.eval_calls))>=0;
    if(valid)valid=JS_SetPropertyStr(ctx,argv[0],"geometryUpdates",JS_NewUint32(ctx,s.geometry_updates))>=0;
    if(valid)valid=JS_SetPropertyStr(ctx,argv[0],"projectionUpdates",JS_NewUint32(ctx,s.projection_updates))>=0;
    if(valid)valid=JS_SetPropertyStr(ctx,argv[0],"shadeUpdates",JS_NewUint32(ctx,s.shade_updates))>=0;
    if(valid)valid=JS_SetPropertyStr(ctx,argv[0],"targetChanged",JS_NewUint32(ctx,s.target_changed))>=0;
    if(valid)valid=JS_SetPropertyStr(ctx,argv[0],"initCopyBytes",JS_NewUint32(ctx,s.init_copy_bytes))>=0;
    if(valid)valid=JS_SetPropertyStr(ctx,argv[0],"targetCopyBytes",JS_NewUint32(ctx,s.target_copy_bytes))>=0;
    a->busy=false;return valid?JS_DupValue(ctx,argv[0]):managed_model_error(ctx,op,AYATSURI_MODEL_INVALID);
}
static JSValue managed_instance_read_pose(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    const char *op="ayatsuri.instance.readPose";pocket_model_actor *a=managed_instance_enter(ctx,self,op,false);
    if(!a)return JS_EXCEPTION;
    double index;shade_view output;ayatsuri_model_status status=AYATSURI_MODEL_INVALID;
    if(argc==2&&number(ctx,argv[0],&index)&&whole(index,0,a->core.model->node_count-1)&&
       managed_model_buffer(ctx,argv[1],JS_TYPED_ARRAY_FLOAT32,true,&output)&&output.bytes==36){
        float pose[9];status=ayatsuri_model_instance_read_pose(&a->core,a,1,(uint16_t)index,pose);
        if(status==AYATSURI_MODEL_OK)memcpy(output.data,pose,sizeof pose);
    }
    a->busy=false;return status==AYATSURI_MODEL_OK?JS_DupValue(ctx,argv[1]):managed_model_error(ctx,op,status);
}
static JSValue managed_model_instance(JSContext *,JSValueConst,int,JSValueConst *);
static const JSCFunctionListEntry managed_model_methods[]={
    JS_CGETSET_DEF("count",managed_model_count,NULL),JS_CFUNC_DEF("node",1,managed_model_node),
    JS_CFUNC_DEF("instance",0,managed_model_instance),JS_CFUNC_DEF("dispose",0,managed_model_dispose)};
static const JSCFunctionListEntry managed_instance_methods[]={
    JS_CFUNC_DEF("targets",4,managed_instance_targets),JS_CFUNC_DEF("evaluate",2,managed_instance_evaluate),
    JS_CFUNC_DEF("submit",0,managed_instance_submit),JS_CFUNC_DEF("stats",1,managed_instance_stats),
    JS_CFUNC_DEF("readPose",2,managed_instance_read_pose),JS_CFUNC_DEF("dispose",0,managed_instance_dispose)};
static const JSClassDef managed_model_definition={.class_name="AyatsuriModel",.finalizer=managed_model_finalizer};
static const JSClassDef managed_instance_definition={.class_name="AyatsuriInstance",.finalizer=managed_instance_finalizer,.gc_mark=managed_instance_mark};
static bool managed_model_class_ready(JSContext *ctx,bool instance){
    JSRuntime *rt=JS_GetRuntime(ctx);JSClassID *id=instance?&pocket_model_instance_class:&pocket_model_class;
    JSRuntime **owner=instance?&pocket_model_instance_runtime:&pocket_model_runtime;
    if(!pocket_api_class_ready(rt,owner,id))JS_NewClassID(rt,id);
    if(!JS_IsRegisteredClass(rt,*id)&&JS_NewClass(rt,*id,instance?&managed_instance_definition:&managed_model_definition)<0){
        if(!JS_HasException(ctx))JS_ThrowOutOfMemory(ctx);
        return false;
    }
    JSValue proto=JS_GetClassProto(ctx,*id);bool ready=!JS_IsNull(proto);JS_FreeValue(ctx,proto);
    if(ready)return true;
    proto=JS_NewObject(ctx);if(JS_IsException(proto))return false;
    const JSCFunctionListEntry *methods=instance?managed_instance_methods:managed_model_methods;
    const int count=instance?(int)(sizeof managed_instance_methods/sizeof *managed_instance_methods):
        (int)(sizeof managed_model_methods/sizeof *managed_model_methods);
    if(JS_SetPropertyFunctionList(ctx,proto,methods,count)<0||JS_HasException(ctx)){JS_FreeValue(ctx,proto);return false;}
    JS_SetClassProto(ctx,*id,proto);return true;
}
static JSValue managed_model_instance(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)argv;const char *op="ayatsuri.model.instance";pocket_model_owner *m=managed_model_enter(ctx,self,op,false);
    if(!m)return JS_EXCEPTION;
    if(argc){m->busy=false;return invalid(ctx,op,"instance takes no arguments");}
    if(!managed_model_class_ready(ctx,true)){m->busy=false;return JS_HasException(ctx)?JS_EXCEPTION:JS_ThrowOutOfMemory(ctx);}
    pocket_model_actor *a=js_mallocz(ctx,sizeof *a);
    if(!a){m->busy=false;return JS_ThrowOutOfMemory(ctx);}
    a->owner=JS_UNDEFINED;a->closed=true;
    const size_t bytes=ayatsuri_model_instance_bytes(&m->model);
    if(bytes>SIZE_MAX-7u){js_free(ctx,a);m->busy=false;return managed_model_error(ctx,op,AYATSURI_MODEL_RANGE);}
    /* The guest heap promises only four-byte alignment on the device. Keep
     * the allocator's original pointer for free, but align the core arena's
     * double-bearing work records explicitly. All padding stays guest-charged. */
    a->arena=js_mallocz(ctx,bytes+7u);
    if(!a->arena){js_free(ctx,a);m->busy=false;return JS_ThrowOutOfMemory(ctx);}
    void *aligned=(void *)(((uintptr_t)a->arena+7u)&~(uintptr_t)7u);
    ayatsuri_model_status status=ayatsuri_model_instance_init(&m->model,aligned,bytes,a,1,&a->core);
    if(status!=AYATSURI_MODEL_OK){js_free(ctx,a->arena);js_free(ctx,a);m->busy=false;return managed_model_error(ctx,op,status);}
    JSValue object=JS_NewObjectClass(ctx,pocket_model_instance_class);
    if(JS_IsException(object)){js_free(ctx,a->arena);js_free(ctx,a);m->busy=false;return object;}
    a->owner=JS_DupValue(ctx,self);a->closed=false;JS_SetOpaque(object,a);m->busy=false;return object;
}
static JSValue managed_model_publish(JSContext *ctx,const uint8_t *data,size_t bytes,uint8_t *owned){
    const char *op="ayatsuri.modelOpen";ayatsuri_model candidate;
    ayatsuri_model_status status=ayatsuri_model_open(data,bytes,&candidate);
    if(status!=AYATSURI_MODEL_OK){js_free(ctx,owned);return managed_model_error(ctx,op,status);}
    if(!managed_model_class_ready(ctx,false)){js_free(ctx,owned);return JS_HasException(ctx)?JS_EXCEPTION:JS_ThrowOutOfMemory(ctx);}
    pocket_model_owner *m=js_mallocz(ctx,sizeof *m);
    if(!m){js_free(ctx,owned);return JS_ThrowOutOfMemory(ctx);}
    m->model=candidate;m->owned=owned;
    JSValue object=JS_NewObjectClass(ctx,pocket_model_class);
    if(JS_IsException(object)){js_free(ctx,owned);js_free(ctx,m);return object;}
    JS_SetOpaque(object,m);return object;
}
JSValue pocket_ayatsuri_model_from_static(JSContext *ctx,const uint8_t *data,size_t bytes){
    return managed_model_publish(ctx,data,bytes,NULL);
}
static JSValue js_ayatsuri_model_open(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;const char *op="ayatsuri.modelOpen";
    /* JS_IsArrayBuffer excludes SharedArrayBuffer in quickjs-ng. Snapshot
     * before validation; subsequent detachment/mutation cannot affect Model. */
    if(argc!=1||!JS_IsArrayBuffer(argv[0]))return invalid(ctx,op,"expected non-shared ArrayBuffer");
    size_t bytes=0;const uint8_t *source=JS_GetArrayBuffer(ctx,&bytes,argv[0]);
    if(!source||bytes>AYATSURI_MODEL_MAX_BYTES)return managed_model_error(ctx,op,bytes>AYATSURI_MODEL_MAX_BYTES?AYATSURI_MODEL_RANGE:AYATSURI_MODEL_INVALID);
    uint8_t *copy=js_malloc(ctx,bytes?bytes:1);if(!copy)return JS_ThrowOutOfMemory(ctx);
    memcpy(copy,source,bytes);return managed_model_publish(ctx,copy,bytes,copy);
}
