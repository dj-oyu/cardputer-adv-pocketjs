/* Real QuickJS explicit Scene skin entry; reuse existing synthetic/reference helpers. */
#define SKIN_JS_MAIN retained_skin_main
#include "test_motion.c"
#undef SKIN_JS_MAIN
int main(int argc,char **argv){
    CHECK(argc==4);strip_rows=(unsigned)atoi(argv[1]);g_ksn_image_rows=atoi(argv[2]);delayed=true;
    JSRuntime *rt=JS_NewRuntime();CHECK(rt);JSContext *ctx=JS_NewContext(rt);CHECK(ctx&&JS_PocketHarden(ctx)==0);JS_SetMemoryLimit(rt,160*1024);
    host_capabilities_clear();CHECK(shiki_heap_begin(0));CHECK(pocket_kasane_install(ctx,NULL)==ESP_OK);CHECK(pocket_shape_install(ctx,NULL)==ESP_OK);
    expose_fixture(ctx,argv[3],"plain","fixture");expose_fixture(ctx,argv[3],"holes","holes");expose_fixture(ctx,argv[3],"sided","sided");
    eval(ctx,"globalThis.pocket={kasane,ayatsuri};globalThis.P=kasane.shiki;const resource=P.resource();kasane.replace(tx=>{tx.background(0x000011ff);tx.image({resource,bounds:[0,0,240,135],clip:[0,0,240,135],sourceWidth:240,sourceHeight:135});})");CHECK(present()==KSN_OK);
    motion_file(ctx,"tools/ayatsuri/skin/js_scenario.js");motion_setup(ctx);
    motion_file(ctx,"apps/lib/ayatsuri_model.js");motion_file(ctx,"tools/ayatsuri/skin/scene_scenario.js");eval(ctx,"sceneSetup()");
    size_t blob_bytes;uint8_t *blob=load_fixture(argv[3],"plain",&blob_bytes);ayatsuri_model model;CHECK(ayatsuri_model_open(blob,blob_bytes,&model)==AYATSURI_MODEL_OK);
    size_t arena_bytes=ayatsuri_model_instance_bytes(&model);void *arena=malloc(arena_bytes);CHECK(arena);unsigned owner=0;ayatsuri_model_instance direct;
    CHECK(ayatsuri_model_instance_init(&model,arena,arena_bytes,&owner,1,&direct)==AYATSURI_MODEL_OK);
    JSValue evaluate=function(ctx,"sceneEvaluate"),draw=function(ctx,"sceneDraw");
    call(ctx,evaluate,JS_NewInt32(ctx,0));uint32_t hashes[120];size_t warm=0;
    for(unsigned visit=0;visit<264;visit++){
        unsigned frame=visit<120?visit:visit<240?239-visit:30;size_t before=alloc_calls;
        call(ctx,evaluate,JS_NewInt32(ctx,(int)frame));warm+=alloc_calls-before;oracle(ctx,&direct,&owner);call(ctx,draw,JS_UNDEFINED);
        if(visit==37){fail_send=true;CHECK(present()==KSN_IO&&pocket_proc_pending());}
        CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
        uint32_t hash=2166136261u;for(unsigned j=0;j<240*135;j++)hash=(hash^panel[j])*16777619u;
        if(visit<120)hashes[frame]=hash;else CHECK(hash==hashes[frame]);
    }
    CHECK(!warm&&hashes[0]!=hashes[30]);
    eval(ctx,"sceneInvalid()");oracle(ctx,&direct,&owner);CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
    eval(ctx,"sceneHidden(false)");CHECK(present()==KSN_OK&&memcmp(panel,reference,sizeof panel));
    eval(ctx,"sceneHidden(true)");CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
    call(ctx,evaluate,JS_NewInt32(ctx,30));oracle(ctx,&direct,&owner);eval(ctx,"G[3]+=100;V[0]+=100;sceneDraw()");CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));eval(ctx,"G[3]-=100;V[0]-=100");
    eval(ctx,"sceneCold()");CHECK(present()==KSN_OK);uint32_t cold=2166136261u;for(unsigned j=0;j<240*135;j++)cold=(cold^panel[j])*16777619u;CHECK(cold==hashes[0]);
    eval(ctx,"sceneFresh()");JS_RunGC(rt);size_t used,limit;JS_GetMemoryCounters(rt,&used,&limit);JS_SetMemoryLimit(rt,used);
    JSValue arg=JS_NewInt32(ctx,30),r=JS_Call(ctx,evaluate,JS_UNDEFINED,1,&arg);JS_SetMemoryLimit(rt,160*1024);CHECK(JS_IsException(r));JSValue e=JS_GetException(ctx);JS_FreeValue(ctx,e);JS_FreeValue(ctx,r);
    eval(ctx,"sceneInspectFailed()");call(ctx,evaluate,arg);oracle(ctx,&direct,&owner);call(ctx,draw,JS_UNDEFINED);CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
    CHECK(ayatsuri_model_instance_dispose(&direct,&owner,1)==AYATSURI_MODEL_OK);free(arena);free(blob);
    eval(ctx,"scenePendingDispose()");JS_RunGC(rt);CHECK(pocket_proc_pending()&&present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
    JS_FreeValue(ctx,evaluate);JS_FreeValue(ctx,draw);pocket_shiki_api_reset();pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);shiki_heap_end();host_capabilities_clear();CHECK(!live_bytes&&!live_blocks);
    printf("SCENE_SKIN_PASS rows=%u direct=%d frames240 pause24 explicit_motion native_skin oracle_pixels warm_alloc0 dirty_visibility cold_restore invalid_busy_oom_recovery pending_source_disposal release0\n",strip_rows,g_ksn_image_rows);return 0;
}
