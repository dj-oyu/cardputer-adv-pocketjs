/* Retain existing real compositor/transport helpers and the single-call tests. */
#define SKIN_JS_MAIN retained_skin_js_main
#include "test_js.c"
#undef SKIN_JS_MAIN

static const float batch_camera[9]={0,0,400,0,0,0,1,120,67},batch_light[6]={0,0,1,1,1,1};
static const size_t batch_bytes=2*sizeof(ayatsuri_model_mesh_geometry)+10*64+3*48+_Alignof(ayatsuri_model_mesh_geometry)-1;
static void batch_oracle(JSContext *ctx,ayatsuri_model_instance *actor,const void *owner,bool subset){
    ayatsuri_transform g[3],bind[3],palette[3];ayatsuri_skin_vertex input[10]={0};ayatsuri_skin_result result[10];
    copy_floats(ctx,"batchGlobals",g,sizeof g);copy_floats(ctx,"batchBind",bind,sizeof bind);
    for(unsigned v=0;v<6;++v){
        input[v].position[0]=(float)((int)(v%3)*30-30);input[v].position[1]=v<3?-15:15;input[v].normal[2]=1;
        input[v].joints[1]=1;input[v].weights[1]=(uint16_t)(v%3==0?0:v%3==1?21845:65535);input[v].weights[0]=(uint16_t)(65535-input[v].weights[1]);
    }
    const float back[8]={-35,-20,35,-20,35,20,-35,20};
    for(unsigned v=0;v<4;++v){input[v+6].position[0]=back[2*v];input[v+6].position[1]=back[2*v+1];input[v+6].normal[2]=1;input[v+6].weights[0]=65535;}
    CHECK(ayatsuri_skin_palette(g,bind,3,palette)==AYATSURI_OK);
    CHECK(ayatsuri_skin_evaluate(palette,2,input,6,result)==AYATSURI_OK);
    CHECK(ayatsuri_skin_evaluate(palette+2,1,input+6,4,result+6)==AYATSURI_OK);
    const ayatsuri_model_mesh_geometry geometry[2]={{1,6,result},{2,4,result+6}};
    CHECK(ayatsuri_model_instance_evaluate_meshes(actor,owner,1,batch_camera,batch_light,geometry,subset?1:2)==AYATSURI_MODEL_OK);
    raster_oracle(actor,owner);
}
static void batch_oom(JSRuntime *rt,JSContext *ctx){
    JSValue attempt=function(ctx,"batchTryB"),fresh=function(ctx,"batchFreshB"),dispose=function(ctx,"batchDisposeB"),inspect=function(ctx,"batchInspectB");
    unsigned failures=0,successes=0;eval(ctx,"batchDisposeB()");
    for(unsigned spare=0;spare<=1536;spare+=64){
        call(ctx,fresh,JS_UNDEFINED);JS_RunGC(rt);size_t used,limit;JS_GetMemoryCounters(rt,&used,&limit);(void)limit;
        JS_SetMemoryLimit(rt,used+spare);JSValue result=JS_Call(ctx,attempt,JS_UNDEFINED,0,NULL);JS_SetMemoryLimit(rt,160*1024);
        bool ok=!JS_IsException(result);if(ok)successes++;else{JSValue e=JS_GetException(ctx);JS_FreeValue(ctx,e);failures++;}
        JS_FreeValue(ctx,result);call(ctx,inspect,JS_NewBool(ctx,ok));call(ctx,dispose,JS_UNDEFINED);
        eval(ctx,"M.stats(MS);check(MS.instances===1)");
    }
    CHECK(failures&&successes);call(ctx,fresh,JS_UNDEFINED);
    JS_FreeValue(ctx,attempt);JS_FreeValue(ctx,fresh);JS_FreeValue(ctx,dispose);JS_FreeValue(ctx,inspect);
    /* Separate single->batch growth: old 480-byte work and ready image survive. */
    eval(ctx,"A.dispose();A=M.instance();batchSingleA();batchInspect(480);batchDraw()");
    CHECK(present()==KSN_OK);memcpy(isolated,panel,sizeof panel);
    attempt=function(ctx,"batchTryA");JS_RunGC(rt);size_t used,limit;JS_GetMemoryCounters(rt,&used,&limit);(void)limit;
    JS_SetMemoryLimit(rt,used);JSValue result=JS_Call(ctx,attempt,JS_UNDEFINED,0,NULL);JS_SetMemoryLimit(rt,160*1024);
    CHECK(JS_IsException(result));JSValue e=JS_GetException(ctx);JS_FreeValue(ctx,e);JS_FreeValue(ctx,result);
    eval(ctx,"batchInspect(480);batchDraw()");CHECK(present()==KSN_OK&&!memcmp(panel,isolated,sizeof panel));
    call(ctx,attempt,JS_UNDEFINED);eval(ctx,"batchInspect(batchBytes)");JS_FreeValue(ctx,attempt);
    printf("SKIN_BATCH_OOM_PASS failures=%u successes=%u single480_growth_atomic recovery=%zu\n",failures,successes,batch_bytes);
}
int main(int argc,char **argv){
    CHECK(argc==4);strip_rows=(unsigned)atoi(argv[1]);g_ksn_image_rows=atoi(argv[2]);delayed=true;CHECK(strip_rows==4||strip_rows==8);
    JSRuntime *rt=JS_NewRuntime();CHECK(rt);JSContext *ctx=JS_NewContext(rt);CHECK(ctx&&JS_PocketHarden(ctx)==0);JS_SetMemoryLimit(rt,160*1024);
    host_capabilities_clear();CHECK(shiki_heap_begin(0));CHECK(pocket_kasane_install(ctx,NULL)==ESP_OK);CHECK(pocket_shape_install(ctx,NULL)==ESP_OK);
    JSValue global=JS_GetGlobalObject(ctx);
    CHECK(JS_SetPropertyStr(ctx,global,"detachBuffer",JS_NewCFunction(ctx,detach_buffer,"detachBuffer",1))>=0);
    CHECK(JS_SetPropertyStr(ctx,global,"freezeBuffer",JS_NewCFunction(ctx,freeze_buffer,"freezeBuffer",1))>=0);
    CHECK(JS_SetPropertyStr(ctx,global,"batchBytes",JS_NewUint32(ctx,(uint32_t)batch_bytes))>=0);
    const size_t near_limit=2*sizeof(ayatsuri_model_mesh_geometry)+64*64+254*48+_Alignof(ayatsuri_model_mesh_geometry)-1;
    CHECK(JS_SetPropertyStr(ctx,global,"batchNearLimit",JS_NewUint32(ctx,(uint32_t)near_limit))>=0);JS_FreeValue(ctx,global);
    install_foreign(ctx,"foreignD",12);install_foreign(ctx,"foreignBG",144);
    expose_fixture(ctx,argv[3],"plain","fixture");expose_fixture(ctx,argv[3],"holes","holes");expose_fixture(ctx,argv[3],"sided","sided");
    expose_fixture(ctx,argv[3],"plain","cycleFixture");expose_fixture(ctx,argv[3],"limit","limitFixture");
    eval(ctx,"globalThis.pocket={kasane,ayatsuri};globalThis.P=kasane.shiki;const resource=P.resource();kasane.replace(tx=>{tx.background(0x000011ff);tx.image({resource,bounds:[0,0,240,135],clip:[0,0,240,135],sourceWidth:240,sourceHeight:135});})");CHECK(present()==KSN_OK);
    FILE *f=fopen("tools/ayatsuri/skin/batch_scenario.js","rb");CHECK(f);static char source[16384];size_t count=fread(source,1,sizeof source-1,f);CHECK(feof(f)&&fclose(f)==0);source[count]=0;eval(ctx,source);eval(ctx,"batchSetup()");
    size_t bytes;uint8_t *blob=load_fixture(argv[3],"plain",&bytes);ayatsuri_model model;CHECK(ayatsuri_model_open(blob,bytes,&model)==AYATSURI_MODEL_OK);
    size_t arena_bytes=ayatsuri_model_instance_bytes(&model);void *arena=malloc(arena_bytes);CHECK(arena);unsigned owner=0;ayatsuri_model_instance direct;
    CHECK(ayatsuri_model_instance_init(&model,arena,arena_bytes,&owner,1,&direct)==AYATSURI_MODEL_OK);
    JSValue evaluate=function(ctx,"batchEvaluate"),draw=function(ctx,"batchDraw");
    call(ctx,evaluate,JS_NewInt32(ctx,0));eval(ctx,"batchInspect(batchBytes)");size_t warm_eval=0,warm_submit=0,guest_peak=0;uint32_t hashes[120];
    for(unsigned visit=0;visit<240;++visit){
        unsigned frame=visit<120?visit:239-visit;size_t before=alloc_calls;
        call(ctx,evaluate,JS_NewInt32(ctx,(int)frame));warm_eval+=alloc_calls-before;batch_oracle(ctx,&direct,&owner,false);
        before=alloc_calls;call(ctx,draw,JS_UNDEFINED);if(visit==37){fail_send=true;CHECK(present()==KSN_IO&&pocket_proc_pending());}
        CHECK(present()==KSN_OK&&!pocket_proc_pending()&&!memcmp(panel,reference,sizeof panel));warm_submit+=alloc_calls-before;
        uint32_t hash=2166136261u;for(unsigned j=0;j<240*135;++j)hash=(hash^panel[j])*16777619u;
        if(visit<120)hashes[frame]=hash;else CHECK(hash==hashes[frame]);
        size_t used,limit;JS_GetMemoryCounters(rt,&used,&limit);CHECK(limit==160*1024);if(used>guest_peak)guest_peak=used;
    }
    CHECK(!warm_eval&&hashes[0]!=hashes[30]);
    call(ctx,evaluate,JS_NewInt32(ctx,30));call(ctx,draw,JS_UNDEFINED);CHECK(present()==KSN_OK);memcpy(isolated,panel,sizeof panel);
    eval(ctx,"batchPose(60);batchTryB();batchDrawB()");batch_oracle(ctx,&direct,&owner,false);CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel)&&memcmp(panel,isolated,sizeof panel));
    eval(ctx,"batchDraw()");CHECK(present()==KSN_OK&&!memcmp(panel,isolated,sizeof panel));
    call(ctx,evaluate,JS_NewInt32(ctx,30));batch_oracle(ctx,&direct,&owner,false);
    eval(ctx,"BV[0]+=100;BG[3]+=100;BD[3]=1;batchDraw()");CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
    eval(ctx,"BV[0]-=100;BG[3]-=100;BD[3]=2;batchLateInvalid()");CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
    eval(ctx,"batchInvalid();batchDraw()");CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
    eval(ctx,"batchAliases()");CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
    batch_oom(rt,ctx);eval(ctx,"batchDraw()");batch_oracle(ctx,&direct,&owner,false);CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
    eval(ctx,"batchSubset()");batch_oracle(ctx,&direct,&owner,true);CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
    eval(ctx,"batchEmpty()");CHECK(ayatsuri_model_instance_evaluate(&direct,&owner,1,batch_camera,batch_light)==AYATSURI_MODEL_OK);raster_oracle(&direct,&owner);CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
    eval(ctx,"batchSingleA();batchInspect(batchBytes);batchDraw()");batch_oracle(ctx,&direct,&owner,true);CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
    call(ctx,evaluate,JS_NewInt32(ctx,30));batch_oracle(ctx,&direct,&owner,false);
    /* Exercise the product Scene facade against the same two-part reference. */
    f=fopen("apps/lib/ayatsuri_model.js","rb");CHECK(f);count=fread(source,1,sizeof source-1,f);CHECK(feof(f)&&fclose(f)==0);source[count]=0;eval(ctx,source);
    eval(ctx,"const S=AyatsuriNative.scene({model:M,camera:{center:[120,67]},light:{direction:[0,0,1]}});function sceneBatchEval(){S.evaluateSkins(0,BD,BG,BI,BV,BJ,BW);}function sceneBatchDraw(){P.beginMeshFrame(17,2048,200000,1);S.render();P.commit();}");
    JSValue se=function(ctx,"sceneBatchEval");
    eval(ctx,"sceneBatchEval()");size_t scene_warm=0;
    for(unsigned frame=0;frame<240;frame++){
        call(ctx,evaluate,JS_NewInt32(ctx,(int)(frame<120?frame:239-frame)));
        size_t before=alloc_calls;call(ctx,se,JS_UNDEFINED);scene_warm+=alloc_calls-before;
        batch_oracle(ctx,&direct,&owner,false);eval(ctx,"sceneBatchDraw()");CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
    }
    CHECK(!scene_warm);
    eval(ctx,"const oldWeight=BW[36];BW[36]=0;rejects(sceneBatchEval);BW[36]=oldWeight;check(!S.busy&&!S.ready);rejects(()=>S.render());sceneBatchEval();rejects(()=>S.evaluateSkins(NaN,BD,BG,BI,BV,BJ,BW));check(S.ready);");
    /* Fresh Scene allocation failure: reject render, clear busy, then recover. */
    eval(ctx,"const T=AyatsuriNative.scene({model:M});function sceneBatchOOM(){T.evaluateSkins(0,BD,BG,BI,BV,BJ,BW);}");
    JSValue so=function(ctx,"sceneBatchOOM");JS_RunGC(rt);size_t used,lim;JS_GetMemoryCounters(rt,&used,&lim);JS_SetMemoryLimit(rt,used);
    JSValue result=JS_Call(ctx,so,JS_UNDEFINED,0,NULL);JS_SetMemoryLimit(rt,160*1024);CHECK(JS_IsException(result));JSValue error=JS_GetException(ctx);JS_FreeValue(ctx,error);JS_FreeValue(ctx,result);
    eval(ctx,"check(!T.busy&&!T.ready);rejects(()=>T.render());sceneBatchOOM();check(T.ready);T.dispose();S.evaluate();sceneBatchDraw();");CHECK(ayatsuri_model_instance_evaluate(&direct,&owner,1,batch_camera,batch_light)==AYATSURI_MODEL_OK);raster_oracle(&direct,&owner);CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
    batch_oracle(ctx,&direct,&owner,false);
    eval(ctx,"sceneBatchEval();sceneBatchDraw();S.dispose();S.dispose();");JS_RunGC(rt);CHECK(pocket_proc_pending()&&present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
    JS_FreeValue(ctx,se);JS_FreeValue(ctx,so);
    printf("SCENE_BATCH_PASS frames=240 warm_alloc=%zu late_invalid_not_ready OOM_recovery pending_dispose\n",scene_warm);
    CHECK(ayatsuri_model_instance_dispose(&direct,&owner,1)==AYATSURI_MODEL_OK);free(arena);free(blob);
    eval(ctx,"batchRelease()");JS_RunGC(rt);CHECK(pocket_proc_pending()&&present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
    eval(ctx,"batchLimits();batchCycle();cycleBatch.stats(MS);check(MS.instances===1);cycleBatch=null");JS_RunGC(rt);
    printf("SKIN_BATCH_HOST rows=%u direct=%d frames=240 meshV=6,4 meshJ=2,1 work=%zu near_limit=%zu ceiling=16384 warm_eval_alloc=%zu submit_present_alloc=%zu guest_frame_peak=%zu requested_peak=%zu\n",strip_rows,g_ksn_image_rows,batch_bytes,near_limit,warm_eval,warm_submit,guest_peak,peak_bytes);
    JS_FreeValue(ctx,evaluate);JS_FreeValue(ctx,draw);pocket_shiki_api_reset();pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);shiki_heap_end();host_capabilities_clear();CHECK(!live_bytes&&!live_blocks);
    puts("SKIN_BATCH_PASS native_multi_mesh duplicate_late_invalid_atomic aliases_reverse detach_alignment busy OOM independent_actor subset_empty_single_restore pending_dispose bounds GC release0");return 0;
}
