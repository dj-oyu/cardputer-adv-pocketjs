/* Host exercises the shipped optional JS entry. Chunk dispatch is a host shim,
 * NOT acceptance of firmware app registry/loader, which needs QEMU/device. */
#define SKIN_JS_MAIN retained_optional_main
#include "test_motion.c"
#undef SKIN_JS_MAIN
static JSValue load_chunk(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;CHECK(argc==1);const char *name=JS_ToCString(ctx,argv[0]);CHECK(name);
    const char *path=!strcmp(name,"motion")?"apps/lib/ayatsuri_motion.js":
      !strcmp(name,"scene")?"apps/lib/ayatsuri_model.js":
      !strcmp(name,"sample")?"apps/skinsample/skin_sample.js":NULL;
    CHECK(path);motion_file(ctx,path);JS_FreeCString(ctx,name);return JS_UNDEFINED;
}
int main(int argc,char **argv){
    CHECK(argc==4);strip_rows=(unsigned)atoi(argv[1]);g_ksn_image_rows=atoi(argv[2]);delayed=true;
    JSRuntime *rt=JS_NewRuntime();CHECK(rt);JSContext *ctx=JS_NewContext(rt);CHECK(ctx&&JS_PocketHarden(ctx)==0);JS_SetMemoryLimit(rt,192*1024);
    host_capabilities_clear();CHECK(shiki_heap_begin(0));CHECK(pocket_kasane_install(ctx,NULL)==ESP_OK);CHECK(pocket_shape_install(ctx,NULL)==ESP_OK);
    expose_fixture(ctx,argv[3],"plain","fixture");
    eval(ctx,"globalThis.pocket={kasane,ayatsuri,app:{}};globalThis.P=kasane.shiki;const resource=P.resource();kasane.replace(tx=>{tx.background(0x000011ff);tx.image({resource,bounds:[0,0,240,135],clip:[0,0,240,135],sourceWidth:240,sourceHeight:135});})");CHECK(present()==KSN_OK);
    JSValue global=JS_GetGlobalObject(ctx),pk=JS_GetPropertyStr(ctx,global,"pocket"),app=JS_GetPropertyStr(ctx,pk,"app");
    CHECK(JS_SetPropertyStr(ctx,app,"load",JS_NewCFunction(ctx,load_chunk,"load",1))>=0);JS_FreeValue(ctx,app);JS_FreeValue(ctx,pk);JS_FreeValue(ctx,global);
    motion_file(ctx,"apps/skinsample/skin_entry.js");
    eval(ctx,"let sample;function start(){sample=startSkinSample(fixture);}function step(t){sample.frame(t);}function stop(){sample.stop();sample.stop();}");
    eval(ctx,"(()=>{const original=AyatsuriNative.scene;let captured,failed=false;AyatsuriNative.scene=options=>{captured=options.model;throw Error('injected startup');};try{startSkinSample(fixture);}catch(e){failed=true;}finally{AyatsuriNative.scene=original;}const stats={};captured.stats(stats);if(!failed||!stats.released||stats.instances!==0)throw Error('startup leak');})();");
    JSValue step=function(ctx,"step");uint32_t hashes[120];size_t warm=0,used=0,limit=0,steady_bytes=0,steady_blocks=0,steady_guest=0,previous_warm=0;
    for(unsigned cycle=0;cycle<3;cycle++){
      warm=0;eval(ctx,"start()");call(ctx,step,JS_NewInt32(ctx,0));CHECK(present()==KSN_OK);
      for(unsigned visit=0;visit<264;visit++){
        unsigned frame=visit<120?visit:visit<240?239-visit:30;size_t before=alloc_calls;
        call(ctx,step,JS_NewInt32(ctx,(int)frame*100));warm+=alloc_calls-before;
        if(visit==37){fail_send=true;CHECK(present()==KSN_IO&&pocket_proc_pending());}
        CHECK(present()==KSN_OK);uint32_t hash=2166136261u;for(unsigned j=0;j<240*135;j++)hash=(hash^panel[j])*16777619u;
        if(!cycle&&visit<120)hashes[frame]=hash;else CHECK(hash==hashes[frame]);
      }
      CHECK(hashes[0]!=hashes[30]);call(ctx,step,JS_NewInt32(ctx,3000));
      eval(ctx,"stop();(()=>{let refused=false;try{sample.frame(0);}catch(e){refused=true;}if(!refused)throw Error('stopped accepted');})();sample=null");
      JS_RunGC(rt);CHECK(pocket_proc_pending()&&present()==KSN_OK);
      uint32_t stopped_hash=2166136261u;for(unsigned j=0;j<240*135;j++)stopped_hash=(stopped_hash^panel[j])*16777619u;CHECK(stopped_hash==hashes[30]);
      JS_GetMemoryCounters(rt,&used,&limit);
      if(cycle){CHECK(live_bytes==steady_bytes&&live_blocks==steady_blocks&&used==steady_guest);if(cycle==2)CHECK(warm==previous_warm);}steady_bytes=live_bytes;steady_blocks=live_blocks;steady_guest=used;previous_warm=warm;
      printf("OPTIONAL_APP_CYCLE cycle=%u guest=%zu limit=%zu tracked=%zu blocks=%zu peak=%zu warmed_frame_alloc=%zu\n",cycle,used,limit,live_bytes,live_blocks,peak_bytes,warm);
    }
    JS_FreeValue(ctx,step);eval(ctx,"fixture=null");
    pocket_shiki_api_reset();pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);shiki_heap_end();host_capabilities_clear();CHECK(!live_bytes&&!live_blocks);
    printf("OPTIONAL_APP_PASS rows=%u direct=%d cycles3 forward_reverse240 pause24 stable_cycle_resources stop_snapshot release0 host_loader_shim\n",strip_rows,g_ksn_image_rows);return 0;
}
