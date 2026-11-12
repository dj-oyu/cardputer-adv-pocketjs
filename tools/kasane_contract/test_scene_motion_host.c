/* Reuse the actual public compositor/transport fixture, not a renderer mock. */
#define main retained_meshbench_main
#include "test_meshbench_app.c"
#undef main
#include "pocket_app_load.h"
#include "app_chunks_host.h"
#include <time.h>
static bool scene_double_sided;
/* The loader logs host elapsed time; no target-cycle result uses this clock. */
int64_t esp_timer_get_time(void){struct timespec t;CHECK(clock_gettime(CLOCK_MONOTONIC,&t)==0);return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000;}

static void load_script(JSContext *ctx,const char *path){
 static char text[16384];FILE *f=fopen(path,"rb");CHECK(f);
 size_t n=fread(text,1,sizeof text-1,f);CHECK(feof(f));fclose(f);text[n]=0;eval(ctx,text);
}
static void phase(JSRuntime *rt,const char *name){
 size_t used,limit;JS_GetMemoryCounters(rt,&used,&limit);
 printf("SCENE_MEMORY phase=%s pointer_bits=%zu requested_live=%zu requested_peak=%zu guest_used=%zu guest_limit=%zu\n",name,sizeof(void*)*8,live_bytes,peak_bytes,used,limit);
}
static void call_time(JSContext *ctx,JSValueConst fn,double time){
 JSValue arg=JS_NewFloat64(ctx,time),result=JS_Call(ctx,fn,JS_UNDEFINED,1,&arg);
 CHECK(!JS_IsException(result));JS_FreeValue(ctx,result);JS_FreeValue(ctx,arg);pocket_kasane_end_turn();
}
static void resources(JSContext *ctx){
 eval(ctx,"S.stats(statsOut);M.stats(modelOut)");JSValue g=JS_GetGlobalObject(ctx),s=JS_GetPropertyStr(ctx,g,"statsOut"),m=JS_GetPropertyStr(ctx,g,"modelOut");
 JSValue a=JS_GetPropertyStr(ctx,s,"arenaBytes"),b=JS_GetPropertyStr(ctx,m,"ownedBytes");int64_t arena=0,cold=0;
 CHECK(JS_ToInt64(ctx,&arena,a)==0&&JS_ToInt64(ctx,&cold,b)==0);
 printf("SCENE_RESOURCES cold=%lld actors=3 arena_per_actor=%lld facade_typed_per_scene=377 scenes=2 included_in_guest_and_requested_totals\n",(long long)cold,(long long)arena);
 JS_FreeValue(ctx,a);JS_FreeValue(ctx,b);JS_FreeValue(ctx,s);JS_FreeValue(ctx,m);JS_FreeValue(ctx,g);
}
static void fixture(JSContext *ctx){
 uint8_t data[2048];FILE *f=fopen(scene_double_sided?".cache/ayatsuri-native/scene-motion-double.aynm":".cache/ayatsuri-native/scene-motion.aynm","rb");CHECK(f);
 size_t n=fread(data,1,sizeof data,f);CHECK(feof(f));fclose(f);
 JSValue g=JS_GetGlobalObject(ctx);CHECK(JS_SetPropertyStr(ctx,g,"fixture",JS_NewArrayBufferCopy(ctx,data,n))>=0);JS_FreeValue(ctx,g);
 eval(ctx,"openModel()");
}
static void oom_scenes(JSRuntime *rt,JSContext *ctx){
 JSValue g=JS_GetGlobalObject(ctx),fn=JS_GetPropertyStr(ctx,g,"attemptScene");JS_FreeValue(ctx,g);
 JSAtom dispose=JS_NewAtom(ctx,"dispose");
 unsigned failed=0,passed=0;
 for(unsigned spare=0;spare<=8192;spare+=128){
  JS_RunGC(rt);size_t used,limit;JS_GetMemoryCounters(rt,&used,&limit);(void)limit;
  JS_SetMemoryLimit(rt,used+spare);
  JSValue scene=JS_Call(ctx,fn,JS_UNDEFINED,0,NULL);JS_SetMemoryLimit(rt,160*1024);
  if(JS_IsException(scene)){JSValue e=JS_GetException(ctx);JS_FreeValue(ctx,e);failed++;}
  else{JSValue result=JS_Invoke(ctx,scene,dispose,0,NULL);CHECK(!JS_IsException(result));JS_FreeValue(ctx,result);passed++;}
  JS_FreeValue(ctx,scene);
  eval(ctx,"M.stats(modelOut);check(modelOut.instances===0)");
  JS_RunGC(rt);
 }
 JS_FreeAtom(ctx,dispose);JS_FreeValue(ctx,fn);CHECK(failed&&passed);
 printf("SCENE_CONSTRUCTOR_OOM_PASS failures=%u successes=%u no_native_actor_before_GC\n",failed,passed);
}
int main(int argc,char **argv){
 CHECK(argc==4);strip_rows=(unsigned)atoi(argv[1]);g_ksn_image_rows=atoi(argv[2]);scene_double_sided=atoi(argv[3])!=0;CHECK(strip_rows==4||strip_rows==8);delayed=true;
 app_registry_select("local.meshbench");CHECK(app_registry_current()&&strcmp(app_registry_current()->id,"local.meshbench")==0);
 size_t host_table_before=live_bytes;CHECK(app_chunks_host_read("tools/ayatsuri/model/scene_motion_chunks.txt")==0);size_t host_table_bytes=live_bytes-host_table_before;
 printf("SCENE_HOST_CHUNKS requested_bytes=%zu file_buffers_names_paths host_only_immutable_flash_in_firmware\n",host_table_bytes);
 JSRuntime *rt=JS_NewRuntime();CHECK(rt);JSContext *ctx=JS_NewContext(rt);CHECK(ctx&&JS_PocketHarden(ctx)==0);JS_SetMemoryLimit(rt,160*1024);
 host_capabilities_clear();CHECK(shiki_heap_begin(0));CHECK(pocket_kasane_install(ctx,NULL)==ESP_OK);CHECK(pocket_shape_install(ctx,NULL)==ESP_OK);
 eval(ctx,"globalThis.pocket={kasane,ayatsuri};globalThis.P=kasane.shiki;const resource=P.resource();kasane.replace(tx=>{tx.background(0x000011ff);tx.image({resource,bounds:[0,0,240,135],clip:[0,0,240,135],sourceWidth:240,sourceHeight:135});})");CHECK(present()==KSN_OK);
 phase(rt,"baseline");CHECK(pocket_app_load_install(ctx)==ESP_OK);
 /* The retained namespace shim contributes eagerly to globals. Match its
  * ordinary pocket alias, while the chunk loader itself is production C. */
 eval(ctx,"pocket.app=app;pocket.app.load('native-scene');pocket.app.load('motion');const sameBuilder=AyatsuriNative;pocket.app.load('native-scene');if(sameBuilder!==AyatsuriNative)throw Error('chunk reload')");
 load_script(ctx,"tools/kasane_contract/scene_motion_scenario.js");phase(rt,"libraries");
 JSValue global=JS_GetGlobalObject(ctx),direct=JS_GetPropertyStr(ctx,global,"tickDirect"),scene=JS_GetPropertyStr(ctx,global,"tickScene");JS_FreeValue(ctx,global);
 static const double times[]={0,1.2,2.8,4.1,10.5,31,79.5,80,40,2.9,2.9,0};
 static uint32_t stable[12];
 for(unsigned cycle=0;cycle<3;cycle++){
  fixture(ctx);if(!cycle)oom_scenes(rt,ctx);eval(ctx,"setupScenes()");phase(rt,"two_scenes_plus_oracle");resources(ctx);size_t warm_scene=0,warm_direct=0;
  for(unsigned k=0;k<120;k++){
   double t=times[k%12];size_t before=alloc_calls;call_time(ctx,direct,t);CHECK(present()==KSN_OK);if(k>=24)warm_direct+=alloc_calls-before;memcpy(reference,panel,sizeof panel);
   if(k%12==0||k%12==2){unsigned red=0,blue=0;for(unsigned i=0;i<240*135;i++){red+=panel[i]==0xf800;blue+=panel[i]==0x001f;}CHECK(red>20);if(k%12==0)CHECK(blue>20);else CHECK(!blue);}
   before=alloc_calls;call_time(ctx,scene,t);
   if(k==37){fail_send=true;CHECK(present()==KSN_IO&&pocket_proc_pending());}
   CHECK(present()==KSN_OK&&!pocket_proc_pending()&&!memcmp(panel,reference,sizeof panel));
   if(k>=24)warm_scene+=alloc_calls-before;
   uint32_t hash=2166136261u;for(unsigned i=0;i<240*135;i++){hash^=panel[i];hash*=16777619u;}
   if(!cycle&&k<12)stable[k]=hash;else CHECK(stable[k%12]==hash);
  }
  CHECK(stable[0]!=stable[4]&&stable[0]!=stable[2]);
  printf("SCENE_WARM_ALLOC cycle=%u frames=96 facade_submit_present=%zu direct_submit_present=%zu no_per_frame_eval_parser\n",cycle,warm_scene,warm_direct);
  for(unsigned mode=1;mode<=3;mode++){
   const char *direct_scale=mode==1?"scaleDirect(1)":mode==2?"scaleDirect(2)":"scaleDirect(0)";
   const char *scene_scale=mode==1?"scaleScene(1)":mode==2?"scaleScene(2)":"scaleScene(0)";
   eval(ctx,direct_scale);CHECK(present()==KSN_OK);memcpy(reference,panel,sizeof panel);
   eval(ctx,scene_scale);CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
   unsigned red=0;for(unsigned i=0;i<240*135;i++)red+=panel[i]==0xf800;
   if(mode==2&&!scene_double_sided)CHECK(!red);else CHECK(red>20);
  }
  printf("SCENE_SCALE_PASS nonuniform reflected=%s restore original_single_winding_preserved\n",scene_double_sided?"explicit_double_visible":"single_culled");
  eval(ctx,"invalidSetters()");CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
  eval(ctx,"noHiddenClock()");CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
  eval(ctx,"failedEvaluate()");CHECK(!pocket_proc_pending());CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
  eval(ctx,"secondScene()");CHECK(present()==KSN_OK&&memcmp(panel,reference,sizeof panel));
  eval(ctx,"recover();callbackGuard()");CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
  eval(ctx,"tickScene(0);closeScenes()");JS_RunGC(rt);phase(rt,"disposed_sources_pending_frame");CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
  eval(ctx,"forgetScenes()");JS_RunGC(rt);phase(rt,"unloaded");
 }
 pocket_proc_mesh_bytes memory;pocket_proc_mesh_memory(&memory);
 printf("SCENE_MOTION_HOST_PASS cycles=3 frames_per_cycle=120 strip=%u direct_rows=%d double=%u requested_peak=%zu snapshot_capacity=%zu workspace=%zu metadata=%zu rigid_hierarchy independent_instances asymmetric_seek pause IO_retry_ACK dispose\n",strip_rows,g_ksn_image_rows,scene_double_sided,peak_bytes,memory.candidate+memory.committed,memory.workspace,memory.metadata);
 JS_FreeValue(ctx,scene);JS_FreeValue(ctx,direct);pocket_shiki_api_reset();pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);shiki_heap_end();host_capabilities_clear();app_chunks_host_clear();CHECK(!live_bytes&&!live_blocks);
 puts("SCENE_MOTION_RELEASE_PASS tracked_live=0 blocks=0");return 0;
}
