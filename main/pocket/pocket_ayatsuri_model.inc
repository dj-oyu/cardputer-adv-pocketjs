#include "ui/kasane/ksn_mesh_depth.h"
/* Immutable cold model + independently owned mutable instances. Include after
 * shade_buffer/fail helpers. No JS backing pointer survives a synchronous call.
 * All native storage is charged to the ordinary guest allocator. */
#include "ayatsuri/ayatsuri_model_skin.h"

typedef struct {
    ayatsuri_model model;
    uint8_t *owned;
    uint32_t references,instances;
    bool closed,busy,released;
} pocket_model_owner;
typedef struct {
    ayatsuri_model_instance core;
    JSValue owner;
    pocket_model_owner *shared;
    void *arena;
    void *skin_work;
    size_t skin_bytes;
    bool closed,busy;
} pocket_model_actor;
static JSClassID pocket_model_class,pocket_model_instance_class;
static pocket_class_owner_t pocket_model_class_owner, pocket_model_instance_class_owner;

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
    bool ok=ordinary&&!JS_HasException(ctx)&&offset<=size&&bytes<=size-offset&&(data||(!size&&!bytes))&&
        (!writable||JS_IsImmutableArrayBuffer(ab)<=0);
    /* Float32/Uint32 paths borrow typed C pointers. Foreign ArrayBuffers may
     * have weaker base alignment than their JS byteOffset suggests. Float64
     * opacity stays byte-addressed in the core and needs no alignment here. */
    if(ok&&(type==JS_TYPED_ARRAY_FLOAT32||type==JS_TYPED_ARRAY_UINT32))
        ok=!data||((uintptr_t)(data+offset)&3u)==0;
    if(ok&&type==JS_TYPED_ARRAY_UINT16)ok=!data||((uintptr_t)(data+offset)&1u)==0;
    if(ok)*out=(shade_view){data?data+offset:NULL,bytes};
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
static void managed_model_detach(JSRuntime *rt,pocket_model_owner *m){
    if(m->closed&&!m->instances&&!m->released){
        js_free_rt(rt,m->owned);m->owned=NULL;
        /* A static span is detached, never freed. No published procedural
         * packet borrows it: submission already copied all numeric fields. */
        m->model.data=NULL;m->released=true;
    }
}
static void managed_model_unref(JSRuntime *rt,pocket_model_owner *m){
    if(--m->references==0){m->closed=true;managed_model_detach(rt,m);js_free_rt(rt,m);}
}
static void managed_instance_release(JSRuntime *rt,pocket_model_actor *a){
    if(!a->closed){
        pocket_model_owner *m=a->shared;
        (void)ayatsuri_model_instance_dispose(&a->core,a,1);
        js_free_rt(rt,a->arena);a->arena=NULL;
        js_free_rt(rt,a->skin_work);a->skin_work=NULL;a->skin_bytes=0;
        JSValue owner=a->owner;a->owner=JS_UNDEFINED;a->shared=NULL;a->closed=true;
        --m->instances;managed_model_detach(rt,m);
        /* JS_FreeValueRT may finalize Model first in a collected expando
         * cycle. Keep the actor's separate C reference until it returns. */
        JS_FreeValueRT(rt,owner);managed_model_unref(rt,m);
    }
}
static void managed_model_finalizer(JSRuntime *rt,JSValue object){
    pocket_model_owner *m=JS_GetOpaque(object,pocket_model_class);
    if(m){m->closed=true;managed_model_detach(rt,m);managed_model_unref(rt,m);}
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
    /* Live actors retain the cold span until their last explicit release. */
    m->closed=true;managed_model_detach(JS_GetRuntime(ctx),m);m->busy=false;return JS_UNDEFINED;
}
static JSValue managed_model_stats(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    const char *op="ayatsuri.model.stats";pocket_model_owner *m=managed_model_enter(ctx,self,op,true);
    if(!m)return JS_EXCEPTION;
    /* Snapshot before invoking any output setter. A setter may legitimately
     * dispose another actor, but cannot reenter this Model while busy. */
    const bool closed=m->closed,released=m->released;
    const uint32_t instances=m->instances;
    const uint32_t owned=!released&&m->owned?m->model.bytes:0;
    const uint32_t external=!released&&!m->owned?m->model.bytes:0;
    bool valid=argc==1&&JS_IsObject(argv[0]);
    if(valid)valid=JS_SetPropertyStr(ctx,argv[0],"closed",JS_NewBool(ctx,closed))>=0;
    if(valid)valid=JS_SetPropertyStr(ctx,argv[0],"instances",JS_NewUint32(ctx,instances))>=0;
    if(valid)valid=JS_SetPropertyStr(ctx,argv[0],"ownedBytes",JS_NewUint32(ctx,owned))>=0;
    if(valid)valid=JS_SetPropertyStr(ctx,argv[0],"staticBytes",JS_NewUint32(ctx,external))>=0;
    if(valid)valid=JS_SetPropertyStr(ctx,argv[0],"released",JS_NewBool(ctx,released))>=0;
    m->busy=false;return valid?JS_DupValue(ctx,argv[0]):managed_model_error(ctx,op,AYATSURI_MODEL_INVALID);
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
/* Keep per-vertex/per-joint work on the guest heap, not the UI task stack.
 * Input access below uses only strict number/typed-array primitives: no JS
 * getters or coercions can detach a previously borrowed input buffer. */
static __attribute__((noinline)) JSValue managed_instance_evaluate_skin(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    const char *op="ayatsuri.instance.evaluateSkin";pocket_model_actor *a=managed_instance_enter(ctx,self,op,false);
    if(!a)return JS_EXCEPTION;
    shade_view v[7];double node;bool valid=argc==8&&number(ctx,argv[2],&node)&&whole(node,0,UINT16_MAX);
    const unsigned args[7]={0,1,3,4,5,6,7};
    const JSTypedArrayEnum types[7]={JS_TYPED_ARRAY_FLOAT32,JS_TYPED_ARRAY_FLOAT32,
        JS_TYPED_ARRAY_FLOAT32,JS_TYPED_ARRAY_FLOAT32,JS_TYPED_ARRAY_FLOAT32,JS_TYPED_ARRAY_UINT16,JS_TYPED_ARRAY_UINT16};
    for(unsigned j=0;valid&&j<7;++j)valid=managed_model_buffer(ctx,argv[args[j]],types[j],false,v+j);
    size_t joints=0,vertices=0;
    if(valid){
        joints=v[2].bytes/48;vertices=v[4].bytes/24;
        valid=v[0].bytes==36&&v[1].bytes==24&&joints>0&&joints<=AYATSURI_MODEL_MAX_NODES&&
            v[2].bytes==joints*48&&v[3].bytes==v[2].bytes&&vertices>=3&&vertices<=KSN_PROC_GOURAUD_VERTICES&&
            v[4].bytes==vertices*24&&v[5].bytes==vertices*8&&v[6].bytes==vertices*8;
    }
    if(!valid){a->busy=false;return managed_model_error(ctx,op,AYATSURI_MODEL_INVALID);}
    const size_t bytes=vertices*(sizeof(ayatsuri_skin_vertex)+sizeof(ayatsuri_skin_result))+joints*sizeof(ayatsuri_transform);
    const bool grow=bytes>a->skin_bytes;void *work=grow?js_malloc(ctx,bytes):a->skin_work;
    if(!work){a->busy=false;return JS_ThrowOutOfMemory(ctx);}
    ayatsuri_skin_vertex *input=work;
    ayatsuri_skin_result *result=(void *)(input+vertices);
    ayatsuri_transform *palette=(void *)(result+vertices);
    ayatsuri_status skin=AYATSURI_OK;
    for(size_t j=0;j<joints&&skin==AYATSURI_OK;++j){
        ayatsuri_transform global,bind;memcpy(&global,v[2].data+j*48,48);memcpy(&bind,v[3].data+j*48,48);
        skin=ayatsuri_skin_palette(&global,&bind,1,palette+j);
    }
    for(size_t j=0;j<vertices;++j){
        memcpy(input[j].position,v[4].data+j*24,12);memcpy(input[j].normal,v[4].data+j*24+12,12);
        memcpy(input[j].joints,v[5].data+j*8,8);memcpy(input[j].weights,v[6].data+j*8,8);
    }
    if(skin==AYATSURI_OK)skin=ayatsuri_skin_evaluate(palette,joints,input,vertices,result);
    ayatsuri_model_status status=skin==AYATSURI_OK?AYATSURI_MODEL_OK:
        skin==AYATSURI_RANGE?AYATSURI_MODEL_RANGE:AYATSURI_MODEL_INVALID;
    if(status==AYATSURI_MODEL_OK){
        float c[9],l[6];memcpy(c,v[0].data,sizeof c);memcpy(l,v[1].data,sizeof l);
        const ayatsuri_model_mesh_geometry geometry={(uint16_t)node,(uint16_t)vertices,result};
        status=ayatsuri_model_instance_evaluate_meshes(&a->core,a,1,c,l,&geometry,1);
    }
    if(grow){
        if(status==AYATSURI_MODEL_OK){js_free(ctx,a->skin_work);a->skin_work=work;a->skin_bytes=bytes;}
        else js_free(ctx,work);
    }
    a->busy=false;return status==AYATSURI_MODEL_OK?JS_UNDEFINED:managed_model_error(ctx,op,status);
}
/* A packed batch avoids getters/array iteration while typed spans are borrowed.
 * All descriptor, copied input, result and palette bytes share the same bounded
 * guest allocation as evaluateSkin; no maximum-node array lives on the stack. */
enum { POCKET_SKIN_BATCH_BYTES=16*1024 };
static uint16_t managed_skin_u16(const uint8_t *p){uint16_t value;memcpy(&value,p,sizeof value);return value;}
static __attribute__((noinline)) JSValue managed_instance_evaluate_skins(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    const char *op="ayatsuri.instance.evaluateSkins";pocket_model_actor *a=managed_instance_enter(ctx,self,op,false);
    if(!a)return JS_EXCEPTION;
    shade_view v[8];bool valid=argc==8;
    const JSTypedArrayEnum types[8]={JS_TYPED_ARRAY_FLOAT32,JS_TYPED_ARRAY_FLOAT32,JS_TYPED_ARRAY_UINT16,
        JS_TYPED_ARRAY_FLOAT32,JS_TYPED_ARRAY_FLOAT32,JS_TYPED_ARRAY_FLOAT32,JS_TYPED_ARRAY_UINT16,JS_TYPED_ARRAY_UINT16};
    for(unsigned j=0;valid&&j<8;++j)valid=managed_model_buffer(ctx,argv[j],types[j],false,v+j);
    size_t count=0,vertices=0,joints=0;
    if(valid){count=v[2].bytes/6;valid=v[0].bytes==36&&v[1].bytes==24&&
        count<=a->core.model->node_count&&v[2].bytes==count*6;}
    for(size_t k=0;valid&&k<count;++k){
        const uint8_t *row=v[2].data+k*6;
        const unsigned id=managed_skin_u16(row),nv=managed_skin_u16(row+2),nj=managed_skin_u16(row+4);
        valid=id<a->core.model->node_count&&nv>=3&&nv<=KSN_PROC_GOURAUD_VERTICES&&nj>0&&nj<=AYATSURI_MODEL_MAX_NODES;
        for(size_t prior=0;valid&&prior<k;++prior)valid=id!=managed_skin_u16(v[2].data+prior*6);
        vertices+=nv;joints+=nj;
    }
    if(valid)valid=v[3].bytes==joints*48&&v[4].bytes==joints*48&&v[5].bytes==vertices*24&&
        v[6].bytes==vertices*8&&v[7].bytes==vertices*8;
    if(!valid){a->busy=false;return managed_model_error(ctx,op,AYATSURI_MODEL_INVALID);}
    float c[9],l[6];memcpy(c,v[0].data,sizeof c);memcpy(l,v[1].data,sizeof l);
    if(!count){
        ayatsuri_model_status status=ayatsuri_model_instance_evaluate(&a->core,a,1,c,l);
        a->busy=false;return status==AYATSURI_MODEL_OK?JS_UNDEFINED:managed_model_error(ctx,op,status);
    }
    const size_t alignment=_Alignof(ayatsuri_model_mesh_geometry),bytes=alignment-1+count*sizeof(ayatsuri_model_mesh_geometry)+
        vertices*(sizeof(ayatsuri_skin_vertex)+sizeof(ayatsuri_skin_result))+joints*sizeof(ayatsuri_transform);
    if(joints>AYATSURI_MODEL_MAX_NODES||bytes>POCKET_SKIN_BATCH_BYTES){
        a->busy=false;return fail(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"skin batch exceeds 256 total joints or 16384 work bytes");
    }
    const bool grow=bytes>a->skin_bytes;void *work=grow?js_malloc(ctx,bytes):a->skin_work;
    if(!work){a->busy=false;return JS_ThrowOutOfMemory(ctx);}
    ayatsuri_model_mesh_geometry *geometry=(void *)(((uintptr_t)work+alignment-1)&~(uintptr_t)(alignment-1));
    ayatsuri_skin_vertex *input=(void *)(geometry+count);
    ayatsuri_skin_result *result=(void *)(input+vertices);
    ayatsuri_transform *palette=(void *)(result+vertices);
    ayatsuri_status skin=AYATSURI_OK;
    for(size_t j=0;j<joints&&skin==AYATSURI_OK;++j){
        ayatsuri_transform global,bind;memcpy(&global,v[3].data+j*48,48);memcpy(&bind,v[4].data+j*48,48);
        skin=ayatsuri_skin_palette(&global,&bind,1,palette+j);
    }
    for(size_t j=0;j<vertices;++j){
        memcpy(input[j].position,v[5].data+j*24,12);memcpy(input[j].normal,v[5].data+j*24+12,12);
        memcpy(input[j].joints,v[6].data+j*8,8);memcpy(input[j].weights,v[7].data+j*8,8);
    }
    size_t vertex=0,joint=0;
    for(size_t k=0;k<count&&skin==AYATSURI_OK;++k){
        const uint8_t *row=v[2].data+k*6;const uint16_t nv=managed_skin_u16(row+2),nj=managed_skin_u16(row+4);
        geometry[k]=(ayatsuri_model_mesh_geometry){managed_skin_u16(row),nv,result+vertex};
        skin=ayatsuri_skin_evaluate(palette+joint,nj,input+vertex,nv,result+vertex);
        vertex+=nv;joint+=nj;
    }
    ayatsuri_model_status status=skin==AYATSURI_OK?AYATSURI_MODEL_OK:
        skin==AYATSURI_RANGE?AYATSURI_MODEL_RANGE:AYATSURI_MODEL_INVALID;
    if(status==AYATSURI_MODEL_OK)status=ayatsuri_model_instance_evaluate_meshes(&a->core,a,1,c,l,geometry,count);
    if(grow){
        if(status==AYATSURI_MODEL_OK){js_free(ctx,a->skin_work);a->skin_work=work;a->skin_bytes=bytes;}
        else js_free(ctx,work);
    }
    a->busy=false;return status==AYATSURI_MODEL_OK?JS_UNDEFINED:managed_model_error(ctx,op,status);
}
static void managed_packet_ellipsoid(const ayatsuri_model_packet *packet,ksn_proc_ellipsoid *p){
    memset(p,0,sizeof *p);
    memcpy(p->screen,packet->storage+96,32);memcpy(p->normal,packet->storage+128,36);
    memcpy(p->material,packet->storage+164,32);memcpy(p->texture,packet->storage+196,36);
    memcpy(p->surface,packet->storage+232,10);memcpy(p->light,packet->lighting,36);
}
static JSValue managed_submit_blend(JSContext *ctx,const char *op,const ayatsuri_model_packet *packet,
        ksn_proc_blend_ellipsoids *p){
    memset(p,0,sizeof *p);p->header.count=packet->lobe_count;
    memcpy(p->header.light,packet->lighting,36);memcpy(p->header.surface,packet->storage+252,6);
    memcpy(p->header.material,packet->storage+264,44);memcpy(p->header.controls,packet->storage+308,8);
    memcpy(p->lobes,packet->storage+316+24*packet->lobe_count,84*packet->lobe_count);
    if(packet->kind==3){
        ksn_proc_blend_region_mask mask;
        _Static_assert(sizeof mask==24,"model blend region tail");
        memcpy(&mask,packet->storage+316+108*packet->lobe_count,sizeof mask);
        return pocket_proc_native_blend_region(ctx,op,p,&mask);
    }
    return pocket_proc_native_blend_ellipsoids(ctx,op,p);
}
/* Emit contiguous side runs in the selected global face order. Grouping all
 * front faces before backs would alter painter order and exact-depth ties. */
static __attribute__((noinline)) JSValue managed_submit_sided_painter(JSContext *ctx,const char *op,
        const ayatsuri_model_sided_mesh_view *v){
    const ksn_proc_gouraud_patch *source=v->patch;ksn_proc_gouraud_patch part;unsigned cursor=0;
    while(cursor<source->header.triangles){
        if(!ksn_mesh_sided_run(source,v->back_colors,v->back_faces,&cursor,&part))return invalid(ctx,op,"invalid sided mesh run");
        JSValue result=pocket_proc_native_gouraud(ctx,op,&part);if(JS_IsException(result))return result;
    }
    return JS_UNDEFINED;
}
static JSValue managed_submit_sided(JSContext *ctx,const char *op,pocket_model_actor *a,uint16_t node){
    ayatsuri_model_sided_mesh_view v;
    ayatsuri_model_status status=ayatsuri_model_instance_sided_mesh_view(&a->core,a,1,node,&v);
    if(status!=AYATSURI_MODEL_OK)return managed_model_error(ctx,op,status);
    if(pocket_proc_native_mesh_depth())return pocket_proc_native_sided_mesh(ctx,op,v.patch,v.depth,v.back_colors,v.back_faces,node);
    return managed_submit_sided_painter(ctx,op,&v);
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
        if(pocket_proc_native_mesh_depth()&&packet.kind!=4&&packet.kind!=5){result=invalid(ctx,op,"opaque mesh scope rejects nonmesh packets and clips");break;}
        if(packet.kind==1){
            ksn_proc_ellipsoid p;managed_packet_ellipsoid(&packet,&p);
            result=pocket_proc_native_ellipsoid(ctx,op,&p);
        }else if(packet.kind==2||packet.kind==3){
            ksn_proc_blend_ellipsoids p;result=managed_submit_blend(ctx,op,&packet,&p);
        }else if(packet.kind==5){
            result=managed_submit_sided(ctx,op,a,packet.node_id);
        }else if(packet.kind==4){
            /* Instance owns the aligned evaluated packet; the frame takes its
             * existing bounded snapshot before this synchronous call returns. */
            if(pocket_proc_native_mesh_depth()){
                ayatsuri_model_mesh_view view;
                ayatsuri_model_status view_status=ayatsuri_model_instance_mesh_view(&a->core,a,1,packet.node_id,&view);
                result=view_status==AYATSURI_MODEL_OK?pocket_proc_native_mesh(ctx,op,view.patch,view.depth,view.node_id):managed_model_error(ctx,op,view_status);
            }else result=pocket_proc_native_gouraud(ctx,op,(const ksn_proc_gouraud_patch *)(packet.storage+84));
        }else result=invalid(ctx,op,"invalid model packet kind");
        if(JS_IsException(result))break;
        status=ayatsuri_model_instance_next(&a->core,a,1,&cursor,&packet);
    }
    if(!JS_IsException(result)&&status!=AYATSURI_MODEL_DONE)result=managed_model_error(ctx,op,status);
    if(JS_IsException(result))pocket_proc_native_rollback(&mark);
    a->busy=false;return result;
}
static uint16_t managed_clip_id(const shade_view *ids,size_t index){
    uint16_t id;memcpy(&id,ids->data+index*2,2);return id;
}
static void managed_packet_clip(const ayatsuri_model_packet *packet,const shade_view *counts,
        const shade_view *xy,size_t index,ksn_proc_clipped_ellipsoid *p){
    managed_packet_ellipsoid(packet,&p->patch);memset(&p->clip,0,sizeof p->clip);
    p->clip.count=counts->data[index];
    if(p->clip.count)memcpy(p->clip.xy,xy->data+index*64,(size_t)p->clip.count*8);
}
static JSValue managed_instance_submit_clipped(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    const char *op="ayatsuri.instance.submitClipped";pocket_model_actor *a=managed_instance_enter(ctx,self,op,false);
    if(!a)return JS_EXCEPTION;
    shade_view ids,counts,xy;
    bool valid=argc==3&&managed_model_buffer(ctx,argv[0],JS_TYPED_ARRAY_UINT16,false,&ids)&&ids.bytes%2==0&&
        managed_model_buffer(ctx,argv[1],JS_TYPED_ARRAY_UINT8,false,&counts)&&
        managed_model_buffer(ctx,argv[2],JS_TYPED_ARRAY_FLOAT32,false,&xy);
    const size_t k=valid?ids.bytes/2:0;
    if(valid)valid=k<=a->core.model->node_count&&counts.bytes==k&&xy.bytes==k*64;
    /* One reusable native packet, never a stack copy of all k clip rows. All
     * source views remain synchronous borrows; no JS runs after validation. */
    union {ksn_proc_clipped_ellipsoid clipped;ksn_proc_blend_ellipsoids blend;} scratch;
    ayatsuri_model_packet packet;ayatsuri_model_status status=AYATSURI_MODEL_OK;
    for(size_t j=0;valid&&j<k;j++){
        const uint16_t id=managed_clip_id(&ids,j);const unsigned count=counts.data[j];
        valid=id<a->core.model->node_count&&(count==0||(count>=3&&count<=8));
        for(size_t previous=0;valid&&previous<j;previous++)if(managed_clip_id(&ids,previous)==id)valid=false;
        if(!valid)break;
        uint32_t at=id;status=ayatsuri_model_instance_next(&a->core,a,1,&at,&packet);
        if(status!=AYATSURI_MODEL_OK||packet.node_id!=id||packet.kind!=1){valid=false;break;}
        managed_packet_clip(&packet,&counts,&xy,j,&scratch.clipped);
        int l,t,r,b;valid=ksn_proc_clipped_ellipsoid_bounds(&scratch.clipped,&l,&t,&r,&b);
    }
    if(!valid){a->busy=false;return managed_model_error(ctx,op,status==AYATSURI_MODEL_STATE?status:AYATSURI_MODEL_INVALID);}
    uint32_t cursor=0;status=ayatsuri_model_instance_next(&a->core,a,1,&cursor,&packet);
    if(status!=AYATSURI_MODEL_OK&&status!=AYATSURI_MODEL_DONE){a->busy=false;return managed_model_error(ctx,op,status);}
    pocket_proc_mark mark;JSValue result=pocket_proc_native_begin(ctx,op,&mark);
    if(JS_IsException(result)){a->busy=false;return result;}
    while(status==AYATSURI_MODEL_OK){
        if(pocket_proc_native_mesh_depth()&&packet.kind!=4&&packet.kind!=5){result=invalid(ctx,op,"opaque mesh scope rejects nonmesh packets and clips");break;}
        size_t selected=k;for(size_t j=0;j<k;j++)if(managed_clip_id(&ids,j)==packet.node_id){selected=j;break;}
        if(selected<k){
            managed_packet_clip(&packet,&counts,&xy,selected,&scratch.clipped);
            result=pocket_proc_native_clipped_ellipsoid(ctx,op,&scratch.clipped);
        }else if(packet.kind==1){
            managed_packet_ellipsoid(&packet,&scratch.clipped.patch);
            result=pocket_proc_native_ellipsoid(ctx,op,&scratch.clipped.patch);
        }else if(packet.kind==2||packet.kind==3){
            result=managed_submit_blend(ctx,op,&packet,&scratch.blend);
        }else if(packet.kind==5){
            result=managed_submit_sided(ctx,op,a,packet.node_id);
        }else if(packet.kind==4){
            /* Instance owns the aligned evaluated packet; the frame takes its
             * existing bounded snapshot before this synchronous call returns. */
            if(pocket_proc_native_mesh_depth()){
                ayatsuri_model_mesh_view view;
                ayatsuri_model_status view_status=ayatsuri_model_instance_mesh_view(&a->core,a,1,packet.node_id,&view);
                result=view_status==AYATSURI_MODEL_OK?pocket_proc_native_mesh(ctx,op,view.patch,view.depth,view.node_id):managed_model_error(ctx,op,view_status);
            }else result=pocket_proc_native_gouraud(ctx,op,(const ksn_proc_gouraud_patch *)(packet.storage+84));
        }else result=invalid(ctx,op,"invalid model packet kind");
        if(JS_IsException(result))break;
        status=ayatsuri_model_instance_next(&a->core,a,1,&cursor,&packet);
    }
    if(!JS_IsException(result)&&status!=AYATSURI_MODEL_DONE)result=managed_model_error(ctx,op,status);
    if(JS_IsException(result))pocket_proc_native_rollback(&mark);
    a->busy=false;return result;
}
static JSValue managed_instance_node_kind(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    const char *op="ayatsuri.instance.nodeKind";pocket_model_actor *a=managed_instance_enter(ctx,self,op,false);
    if(!a)return JS_EXCEPTION;
    double index;uint8_t kind=0;ayatsuri_model_status status=AYATSURI_MODEL_INVALID;
    if(argc==1&&number(ctx,argv[0],&index)&&whole(index,0,UINT16_MAX))
        status=ayatsuri_model_instance_node_kind(&a->core,a,1,(uint16_t)index,&kind);
    a->busy=false;return status==AYATSURI_MODEL_OK?JS_NewUint32(ctx,kind):managed_model_error(ctx,op,status);
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
    if(valid)valid=JS_SetPropertyStr(ctx,argv[0],"skinWorkBytes",JS_NewFloat64(ctx,(double)a->skin_bytes))>=0;
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
static JSValue managed_instance_read_projection(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    const char *op="ayatsuri.instance.readProjection";pocket_model_actor *a=managed_instance_enter(ctx,self,op,false);
    if(!a)return JS_EXCEPTION;
    double index;shade_view output;ayatsuri_model_status status=AYATSURI_MODEL_INVALID;
    if(argc==2&&number(ctx,argv[0],&index)&&whole(index,0,a->core.model->node_count-1)&&
       managed_model_buffer(ctx,argv[1],JS_TYPED_ARRAY_FLOAT32,true,&output)&&output.bytes==32){
        uint32_t cursor=(uint32_t)index;ayatsuri_model_packet packet;
        status=ayatsuri_model_instance_next(&a->core,a,1,&cursor,&packet);
        if(status==AYATSURI_MODEL_OK&&packet.node_id==(uint16_t)index&&packet.kind==1)memcpy(output.data,packet.storage+96,32);
        else if(status!=AYATSURI_MODEL_STATE)status=AYATSURI_MODEL_INVALID;
    }
    a->busy=false;return status==AYATSURI_MODEL_OK?JS_DupValue(ctx,argv[1]):managed_model_error(ctx,op,status);
}
static JSValue managed_model_instance(JSContext *,JSValueConst,int,JSValueConst *);
static const JSCFunctionListEntry managed_model_methods[]={
    JS_CGETSET_DEF("count",managed_model_count,NULL),JS_CFUNC_DEF("node",1,managed_model_node),
    JS_CFUNC_DEF("instance",0,managed_model_instance),JS_CFUNC_DEF("stats",1,managed_model_stats),
    JS_CFUNC_DEF("dispose",0,managed_model_dispose)};
static const JSCFunctionListEntry managed_instance_methods[]={
    JS_CFUNC_DEF("targets",4,managed_instance_targets),JS_CFUNC_DEF("evaluate",2,managed_instance_evaluate),
    JS_CFUNC_DEF("evaluateSkin",8,managed_instance_evaluate_skin),
    JS_CFUNC_DEF("evaluateSkins",8,managed_instance_evaluate_skins),
    JS_CFUNC_DEF("nodeKind",1,managed_instance_node_kind),
    JS_CFUNC_DEF("submit",0,managed_instance_submit),JS_CFUNC_DEF("stats",1,managed_instance_stats),
    JS_CFUNC_DEF("submitClipped",3,managed_instance_submit_clipped),
    JS_CFUNC_DEF("readProjection",2,managed_instance_read_projection),
    JS_CFUNC_DEF("readPose",2,managed_instance_read_pose),JS_CFUNC_DEF("dispose",0,managed_instance_dispose)};
static const JSClassDef managed_model_definition={.class_name="AyatsuriModel",.finalizer=managed_model_finalizer};
static const JSClassDef managed_instance_definition={.class_name="AyatsuriInstance",.finalizer=managed_instance_finalizer,.gc_mark=managed_instance_mark};
static bool managed_model_class_ready(JSContext *ctx,bool instance){
    JSRuntime *rt=JS_GetRuntime(ctx);JSClassID *id=instance?&pocket_model_instance_class:&pocket_model_class;
    pocket_class_owner_t *owner=instance?&pocket_model_instance_class_owner:&pocket_model_class_owner;
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
    if(m->references==UINT32_MAX||m->instances==UINT32_MAX){m->busy=false;return managed_model_error(ctx,op,AYATSURI_MODEL_RANGE);}
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
    ++m->references;++m->instances;a->shared=m;
    a->owner=JS_DupValue(ctx,self);a->closed=false;JS_SetOpaque(object,a);m->busy=false;return object;
}
static JSValue managed_model_publish(JSContext *ctx,const uint8_t *data,size_t bytes,uint8_t *owned){
    const char *op="ayatsuri.modelOpen";ayatsuri_model candidate;
    ayatsuri_model_status status=ayatsuri_model_open(data,bytes,&candidate);
    if(status!=AYATSURI_MODEL_OK){js_free(ctx,owned);return managed_model_error(ctx,op,status);}
    if(!managed_model_class_ready(ctx,false)){js_free(ctx,owned);return JS_HasException(ctx)?JS_EXCEPTION:JS_ThrowOutOfMemory(ctx);}
    pocket_model_owner *m=js_mallocz(ctx,sizeof *m);
    if(!m){js_free(ctx,owned);return JS_ThrowOutOfMemory(ctx);}
    m->model=candidate;m->owned=owned;m->references=1;
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
