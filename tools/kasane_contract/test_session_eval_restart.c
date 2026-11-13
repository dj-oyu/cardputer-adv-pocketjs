/* Real QuickJS finalizers and real Kasane/Shiki resets, not app_start itself. */
#define main retained_meshbench_main
#include "test_meshbench_app.c"
#undef main

static void one_session(unsigned fail_stage) {
    CHECK(!live_bytes && !live_blocks);
    CHECK(shiki_heap_begin(0));
    JSRuntime *rt=JS_NewRuntime();CHECK(rt);
    JSContext *ctx=JS_NewContext(rt);CHECK(ctx&&JS_PocketHarden(ctx)==0);
    JS_SetMemoryLimit(rt,160*1024);
    host_capabilities_clear();
    CHECK(pocket_kasane_install(ctx,NULL)==ESP_OK);
    CHECK(pocket_shape_install(ctx,NULL)==ESP_OK);
    const char *resource="globalThis.P=kasane.shiki;globalThis.r=P.resource();globalThis.w=P.data({type:'i16',q:0,records:2,record:2});globalThis.fields=P.fields(w);fields[0][0]=7;";
    const char *attach="kasane.replace(tx=>{tx.background(0x000011ff);tx.image({resource:r,bounds:[0,0,240,135],clip:[0,0,240,135],sourceWidth:240,sourceHeight:135});});";
    char source[1024];
    snprintf(source,sizeof source,"%s%s%s",
             fail_stage>=1?resource:"", fail_stage>=2?attach:"",
             fail_stage==3?"":"throw Error('intentional startup rejection');");
    JSValue result=JS_Eval(ctx,source,strlen(source),"synthetic-startup",JS_EVAL_TYPE_GLOBAL);
    CHECK(JS_IsException(result)==(fail_stage!=3));
    if(JS_IsException(result)) {
        JSValue exception=JS_GetException(ctx);
        const char *message=JS_ToCString(ctx,exception);CHECK(message);
        CHECK(strstr(message,"intentional startup rejection"));
        JS_FreeCString(ctx,message);JS_FreeValue(ctx,exception);
    }
    JS_FreeValue(ctx,result);
    pocket_kasane_end_turn();
    if(fail_stage==3)CHECK(present()==KSN_OK);
    CHECK(pocket_kasane_needs_present()==(fail_stage==2));
    unsigned budget,used,largest;shiki_heap_info(&budget,&used,&largest);
    if(fail_stage>=1)CHECK(used>0);
    printf("REAL_EVAL phase=%u heap_used=%u requested_live=%zu pending=%d\n",
           fail_stage,used,live_bytes,pocket_kasane_needs_present());
    /* Match the relative product app_stop ordering. Peripheral/API resets
     * and pocketjs_guest wrapper are not part of this harness. */
    CHECK(pocket_kasane_reset());
    pocket_shiki_api_reset();
    if(fail_stage>=1)eval(ctx,"if(w.byteLength!==0||fields[0].byteLength!==0||fields[1].byteLength!==0)throw Error('window not detached')");
    JS_FreeContext(ctx);JS_FreeRuntime(rt);
    shiki_heap_end();host_capabilities_clear();
    CHECK(!shiki_heap_active()&&!live_bytes&&!live_blocks);
    CHECK(pocket_kasane_reset());pocket_shiki_api_reset();shiki_heap_end();
    CHECK(!live_bytes&&!live_blocks);
}
int main(void) {
    strip_rows=4;g_ksn_image_rows=0;delayed=true;
    app_registry_select("local.meshbench");
    for(unsigned repeat=0;repeat<3;repeat++) {
        for(unsigned stage=0;stage<3;stage++) {
            one_session(stage);
            one_session(3);
        }
    }
    puts("REAL_EVAL_RESTART_PASS rejected=9 successful_restarts=9 real_finalizers_real_resets tracked_live=0");
    return 0;
}
