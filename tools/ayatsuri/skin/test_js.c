/* Actual QuickJS, native submit, compositor and delayed transport fixture. */
#define main retained_meshbench_main
#include "../../kasane_contract/test_meshbench_app.c"
#undef main
#include "ayatsuri/ayatsuri_model_skin.h"
static uint16_t isolated[240*135];

static JSValue detach_buffer(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;CHECK(argc==1);JS_DetachArrayBuffer(ctx,argv[0]);return JS_UNDEFINED;
}
static JSValue freeze_buffer(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)ctx;(void)self;CHECK(argc==1&&JS_SetImmutableArrayBuffer(argv[0],true)==0);return JS_UNDEFINED;
}
static void foreign_free(JSRuntime *rt,void *opaque,void *data){(void)rt;(void)data;free(opaque);}
static void install_foreign(JSContext *ctx,const char *name,size_t n){
    uint8_t *p=malloc(n+1);CHECK(p);memset(p,0,n+1);
    JSValue b=JS_NewArrayBuffer(ctx,p+1,n,foreign_free,p,false);CHECK(!JS_IsException(b));
    JSValue g=JS_GetGlobalObject(ctx);CHECK(JS_SetPropertyStr(ctx,g,name,b)>=0);JS_FreeValue(ctx,g);
}
static uint8_t *load_fixture(const char *dir,const char *name,size_t *bytes){
    char path[1024];CHECK(snprintf(path,sizeof path,"%s/skin-%s.aynm",dir,name)>0);
    FILE *f=fopen(path,"rb");CHECK(f&&fseek(f,0,SEEK_END)==0);long n=ftell(f);CHECK(n>0);rewind(f);
    uint8_t *p=malloc((size_t)n);CHECK(p&&fread(p,1,(size_t)n,f)==(size_t)n&&fclose(f)==0);*bytes=(size_t)n;return p;
}
static void expose_fixture(JSContext *ctx,const char *dir,const char *variant,const char *name){
    size_t n;uint8_t *p=load_fixture(dir,variant,&n);JSValue g=JS_GetGlobalObject(ctx);
    CHECK(JS_SetPropertyStr(ctx,g,name,JS_NewArrayBufferCopy(ctx,p,n))>=0);JS_FreeValue(ctx,g);free(p);
}
static JSValue function(JSContext *ctx,const char *name){
    JSValue g=JS_GetGlobalObject(ctx),f=JS_GetPropertyStr(ctx,g,name);JS_FreeValue(ctx,g);CHECK(JS_IsFunction(ctx,f));return f;
}
static void call(JSContext *ctx,JSValueConst fn,JSValueConst arg){
    JSValue value=JS_Call(ctx,fn,JS_UNDEFINED,1,&arg);
    if(JS_IsException(value)){JSValue e=JS_GetException(ctx);const char *s=JS_ToCString(ctx,e);fprintf(stderr,"call: %s\n",s?s:"exception");CHECK(0);}
    JS_FreeValue(ctx,value);pocket_kasane_end_turn();
}
static void copy_floats(JSContext *ctx,const char *name,void *out,size_t n){
    JSValue g=JS_GetGlobalObject(ctx),v=JS_GetPropertyStr(ctx,g,name);JS_FreeValue(ctx,g);
    size_t offset,bytes,per,size;JSValue ab=JS_GetTypedArrayBuffer(ctx,v,&offset,&bytes,&per);
    uint8_t *p=JS_GetArrayBuffer(ctx,&size,ab);CHECK(p&&bytes==n&&offset<=size&&bytes<=size-offset);memcpy(out,p+offset,n);
    JS_FreeValue(ctx,ab);JS_FreeValue(ctx,v);
}
static void raster_oracle(ayatsuri_model_instance *actor,const void *owner){
    uint8_t bytes[2048];float scratch[240];ksn_mesh_snapshot s={.packets={bytes,sizeof bytes,0,0},.enabled=true,.work_limit=200000};
    uint32_t cursor=0;ayatsuri_model_packet packet;ayatsuri_model_status status;
    while((status=ayatsuri_model_instance_next(actor,owner,1,&cursor,&packet))==AYATSURI_MODEL_OK){
        ayatsuri_model_mesh_view v;CHECK(packet.kind==4&&ayatsuri_model_instance_mesh_view(actor,owner,1,packet.node_id,&v)==AYATSURI_MODEL_OK);
        CHECK(ksn_mesh_append(&s,v.patch,v.depth,v.node_id));
    }
    CHECK(status==AYATSURI_MODEL_DONE);for(unsigned j=0;j<240*135;++j)reference[j]=17;
    CHECK(ksn_mesh_render(&s,reference,0,135,scratch,1));
}
static void second_oracle(ayatsuri_model_instance *actor,const void *owner){
    const ayatsuri_skin_result v[4]={{{-27,-20,9},{0,0,1}},{{43,-20,9},{0,0,1}},{{43,20,9},{0,0,1}},{{-27,20,9},{0,0,1}}};
    const float c[9]={0,0,400,0,0,0,1,120,67},l[6]={0,0,1,1,1,1};const ayatsuri_model_mesh_geometry g={2,4,v};
    CHECK(ayatsuri_model_instance_evaluate_meshes(actor,owner,1,c,l,&g,1)==AYATSURI_MODEL_OK);raster_oracle(actor,owner);
}
static void oracle(JSContext *ctx,ayatsuri_model_instance *actor,const void *owner){
    ayatsuri_transform g[2],bind[2],palette[2];ayatsuri_skin_vertex input[6]={0};ayatsuri_skin_result result[6];
    copy_floats(ctx,"skinGlobals",g,sizeof g);copy_floats(ctx,"skinInverseBind",bind,sizeof bind);
    for(unsigned v=0;v<6;++v){
        input[v].position[0]=(float)((int)(v%3)*30-30);input[v].position[1]=v<3?-15:15;input[v].normal[2]=1;
        input[v].joints[1]=1;input[v].weights[1]=(uint16_t)(v%3==0?0:v%3==1?21845:65535);input[v].weights[0]=(uint16_t)(65535-input[v].weights[1]);
    }
    CHECK(ayatsuri_skin_palette(g,bind,2,palette)==AYATSURI_OK&&ayatsuri_skin_evaluate(palette,2,input,6,result)==AYATSURI_OK);
    const float c[9]={0,0,400,0,0,0,1,120,67},l[6]={0,0,1,1,1,1};const ayatsuri_model_mesh_geometry geometry={1,6,result};
    CHECK(ayatsuri_model_instance_evaluate_meshes(actor,owner,1,c,l,&geometry,1)==AYATSURI_MODEL_OK);
    raster_oracle(actor,owner);
}
static void oom(JSRuntime *rt,JSContext *ctx){
    JSValue attempt=function(ctx,"attemptSkin"),fresh=function(ctx,"freshB"),dispose=function(ctx,"disposeB"),inspect=function(ctx,"inspectB");
    unsigned failures=0,successes=0;
    eval(ctx,"disposeB()");
    for(unsigned spare=0;spare<=1024;spare+=32){
        call(ctx,fresh,JS_UNDEFINED);JS_RunGC(rt);size_t used,limit;JS_GetMemoryCounters(rt,&used,&limit);(void)limit;
        JS_SetMemoryLimit(rt,used+spare);JSValue result=JS_Call(ctx,attempt,JS_UNDEFINED,0,NULL);
        JS_SetMemoryLimit(rt,160*1024);const bool ok=!JS_IsException(result);
        if(!ok){JSValue e=JS_GetException(ctx);JS_FreeValue(ctx,e);failures++;}else successes++;
        JS_FreeValue(ctx,result);call(ctx,inspect,JS_NewBool(ctx,ok));call(ctx,dispose,JS_UNDEFINED);
        eval(ctx,"M.stats(modelStats);check(modelStats.instances===1)");
    }
    CHECK(failures&&successes);call(ctx,fresh,JS_UNDEFINED);
    JS_FreeValue(ctx,attempt);JS_FreeValue(ctx,fresh);JS_FreeValue(ctx,dispose);JS_FreeValue(ctx,inspect);
    eval(ctx,"inspectA(480)");JSValue grow=function(ctx,"attemptGrowth");JS_RunGC(rt);size_t used,limit;JS_GetMemoryCounters(rt,&used,&limit);
    JS_SetMemoryLimit(rt,used);JSValue result=JS_Call(ctx,grow,JS_UNDEFINED,0,NULL);JS_SetMemoryLimit(rt,160*1024);CHECK(JS_IsException(result));
    JSValue e=JS_GetException(ctx);JS_FreeValue(ctx,e);JS_FreeValue(ctx,result);eval(ctx,"inspectA(480)");
    call(ctx,grow,JS_UNDEFINED);eval(ctx,"inspectA(576)");JS_FreeValue(ctx,grow);
    printf("SKIN_JS_OOM_PASS initial_failures=%u successes=%u initial_ready_preserved growth480_preserved recovery576 instances_stable\n",failures,successes);
}
int main(int argc,char **argv){
    CHECK(argc==4);strip_rows=(unsigned)atoi(argv[1]);g_ksn_image_rows=atoi(argv[2]);delayed=true;CHECK(strip_rows==4||strip_rows==8);
    JSRuntime *rt=JS_NewRuntime();CHECK(rt);JSContext *ctx=JS_NewContext(rt);CHECK(ctx&&JS_PocketHarden(ctx)==0);JS_SetMemoryLimit(rt,160*1024);
    host_capabilities_clear();CHECK(shiki_heap_begin(0));CHECK(pocket_kasane_install(ctx,NULL)==ESP_OK);CHECK(pocket_shape_install(ctx,NULL)==ESP_OK);
    JSValue g=JS_GetGlobalObject(ctx);CHECK(JS_SetPropertyStr(ctx,g,"detachBuffer",JS_NewCFunction(ctx,detach_buffer,"detachBuffer",1))>=0);
    CHECK(JS_SetPropertyStr(ctx,g,"freezeBuffer",JS_NewCFunction(ctx,freeze_buffer,"freezeBuffer",1))>=0);JS_FreeValue(ctx,g);
    install_foreign(ctx,"foreignJ",48);install_foreign(ctx,"foreignG",96);
    expose_fixture(ctx,argv[3],"plain","fixture");expose_fixture(ctx,argv[3],"holes","holes");expose_fixture(ctx,argv[3],"sided","sided");expose_fixture(ctx,argv[3],"plain","cycleFixture");
    eval(ctx,"globalThis.pocket={kasane,ayatsuri};globalThis.P=kasane.shiki;const resource=P.resource();kasane.replace(tx=>{tx.background(0x000011ff);tx.image({resource,bounds:[0,0,240,135],clip:[0,0,240,135],sourceWidth:240,sourceHeight:135});})");CHECK(present()==KSN_OK);
    FILE *f=fopen("tools/ayatsuri/skin/js_scenario.js","rb");CHECK(f);static char source[16384];size_t count=fread(source,1,sizeof source-1,f);CHECK(feof(f)&&fclose(f)==0);source[count]=0;eval(ctx,source);eval(ctx,"setup()");
    size_t blob_bytes;uint8_t *blob=load_fixture(argv[3],"plain",&blob_bytes);ayatsuri_model model;CHECK(ayatsuri_model_open(blob,blob_bytes,&model)==AYATSURI_MODEL_OK);
    size_t arena_bytes=ayatsuri_model_instance_bytes(&model);void *arena=malloc(arena_bytes);CHECK(arena);unsigned owner=0;ayatsuri_model_instance direct;
    CHECK(ayatsuri_model_instance_init(&model,arena,arena_bytes,&owner,1,&direct)==AYATSURI_MODEL_OK);
    JSValue evaluate=function(ctx,"evaluateOnly"),draw=function(ctx,"drawA"),tick_fn=function(ctx,"tick");
    call(ctx,evaluate,JS_NewInt32(ctx,0));eval(ctx,"inspectA(480)");size_t warm_evaluate=0,warm_submit=0,guest_peak=0;uint32_t hashes[120];
    for(unsigned visit=0;visit<240;++visit){
        unsigned frame=visit<120?visit:239-visit;size_t before=alloc_calls;
        call(ctx,evaluate,JS_NewInt32(ctx,(int)frame));warm_evaluate+=alloc_calls-before;
        oracle(ctx,&direct,&owner);before=alloc_calls;call(ctx,draw,JS_UNDEFINED);
        if(visit==37){fail_send=true;CHECK(present()==KSN_IO&&pocket_proc_pending());}
        CHECK(present()==KSN_OK&&!pocket_proc_pending()&&!memcmp(panel,reference,sizeof panel));warm_submit+=alloc_calls-before;
        uint32_t hash=2166136261u;for(unsigned j=0;j<240*135;++j)hash=(hash^panel[j])*16777619u;
        if(visit<120)hashes[frame]=hash;else CHECK(hash==hashes[frame]);
        size_t used,limit;JS_GetMemoryCounters(rt,&used,&limit);CHECK(limit==160*1024);if(used>guest_peak)guest_peak=used;
    }
    CHECK(!warm_evaluate&&hashes[0]!=hashes[30]);
    eval(ctx,"drawB()");CHECK(present()==KSN_OK);uint32_t rigid_hash=2166136261u;for(unsigned j=0;j<240*135;++j)rigid_hash=(rigid_hash^panel[j])*16777619u;CHECK(rigid_hash==hashes[0]);
    /* Both actors own separate skin work and cached packets. */
    call(ctx,tick_fn,JS_NewInt32(ctx,30));CHECK(present()==KSN_OK);memcpy(isolated,panel,sizeof panel);
    eval(ctx,"poseAt(60);B.evaluateSkin(C,L,1,G,IB,V,J,W);drawB()");oracle(ctx,&direct,&owner);
    CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel)&&memcmp(panel,isolated,sizeof panel));
    eval(ctx,"drawA()");CHECK(present()==KSN_OK&&!memcmp(panel,isolated,sizeof panel));
    eval(ctx,"B.evaluate(C,L)");
    /* JS backing arrays can change after evaluation, before any submission. */
    call(ctx,evaluate,JS_NewInt32(ctx,30));oracle(ctx,&direct,&owner);
    eval(ctx,"G[3]+=100;V[0]+=100;drawA()");CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
    eval(ctx,"G[3]-=100;V[0]-=100;invalidInputs();drawA()");oracle(ctx,&direct,&owner);
    CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
    oom(rt,ctx);call(ctx,tick_fn,JS_NewInt32(ctx,30));oracle(ctx,&direct,&owner);CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
    eval(ctx,"inspectA(576);overrideSecond()");second_oracle(&direct,&owner);
    CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
    call(ctx,tick_fn,JS_NewInt32(ctx,30));oracle(ctx,&direct,&owner);
    CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));eval(ctx,"inspectA(576)");
    puts("SKIN_JS_SCOPE_PASS smaller_reuses576 one_override_only different_node_restores_cold");
    eval(ctx,"A.evaluate(C,L);inspectA(576);drawA()");CHECK(present()==KSN_OK);uint32_t restored=2166136261u;for(unsigned j=0;j<240*135;++j)restored=(restored^panel[j])*16777619u;CHECK(restored==hashes[0]);
    eval(ctx,"closeModel()");call(ctx,evaluate,JS_NewInt32(ctx,30));oracle(ctx,&direct,&owner);
    CHECK(ayatsuri_model_instance_dispose(&direct,&owner,1)==AYATSURI_MODEL_OK);free(arena);free(blob);
    eval(ctx,"pendingThenDispose()");JS_RunGC(rt);CHECK(pocket_proc_pending()&&present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
    eval(ctx,"implicitRelease();cycleModel.stats(modelStats);check(modelStats.instances===1);cycleModel=null");JS_RunGC(rt);
    pocket_proc_mesh_bytes memory;pocket_proc_mesh_memory(&memory);
    printf("SKIN_JS_HOST rows=%u direct=%d frames=240 warm_evaluate_alloc=%zu submit_present_alloc=%zu guest_frame_peak=%zu requested_live=%zu requested_peak=%zu C_oracle_cold=%zu C_oracle_arena=%zu snapshot_capacity=%zu workspace=%zu\n",strip_rows,g_ksn_image_rows,warm_evaluate,warm_submit,guest_peak,live_bytes,peak_bytes,blob_bytes,arena_bytes,memory.candidate+memory.committed,memory.workspace);
    JS_FreeValue(ctx,evaluate);JS_FreeValue(ctx,draw);JS_FreeValue(ctx,tick_fn);
    pocket_shiki_api_reset();pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);shiki_heap_end();host_capabilities_clear();CHECK(!live_bytes&&!live_blocks);
    puts("SKIN_JS_PASS strict_buffers invalid_atomic SAB_detached_alignment immutable_input busy_recovery independent_actor cold_restore IO_retry pending_source_disposal cyclic_GC release0");return 0;
}
