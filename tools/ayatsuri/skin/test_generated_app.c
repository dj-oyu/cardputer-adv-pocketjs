/* Generated Blender model/arrays -> real QuickJS Scene/binding -> renderer. */
#define SKIN_JS_MAIN retained_skin_main
#include "test_js.c"
#undef SKIN_JS_MAIN
static void bridge_file(JSContext *ctx,const char *path){
 FILE *f=fopen(path,"rb");CHECK(f);char source[65536];size_t n=fread(source,1,sizeof source-1,f);CHECK(feof(f)&&!ferror(f)&&fclose(f)==0);source[n]=0;eval(ctx,source);
}
static bool geometry_close(const ayatsuri_skin_result *actual,const ayatsuri_skin_result *expected){
 for(unsigned v=0;v<6;v++)for(unsigned k=0;k<3;k++)if(!isfinite(actual[v].position[k])||!isfinite(actual[v].normal[k])||fabsf(actual[v].position[k]-expected[v].position[k])>2e-5f||fabsf(actual[v].normal[k]-expected[v].normal[k])>2e-5f)return false;
 return true;
}
static JSValue load_generated_chunk(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
 (void)self;CHECK(argc==1);const char *name=JS_ToCString(ctx,argv[0]);CHECK(name);
 const char *path=!strcmp(name,"scene")?"apps/lib/ayatsuri_model.js":!strcmp(name,"sample")?"apps/skinasset/generated_skin_sample.js":NULL;
 CHECK(path);bridge_file(ctx,path);JS_FreeCString(ctx,name);return JS_UNDEFINED;
}
static int run_cycle(int argc,char **argv){
 CHECK(argc==4);strip_rows=(unsigned)atoi(argv[1]);g_ksn_image_rows=atoi(argv[2]);delayed=true;
 JSRuntime *rt=JS_NewRuntime();CHECK(rt);JSContext *ctx=JS_NewContext(rt);CHECK(ctx&&JS_PocketHarden(ctx)==0);JS_SetMemoryLimit(rt,160*1024);
 host_capabilities_clear();CHECK(shiki_heap_begin(0));CHECK(pocket_kasane_install(ctx,NULL)==ESP_OK);CHECK(pocket_shape_install(ctx,NULL)==ESP_OK);
 expose_fixture(ctx,argv[3],"plain","fixture");
 eval(ctx,"globalThis.pocket={kasane,ayatsuri};globalThis.P=kasane.shiki;const resource=P.resource();kasane.replace(tx=>{tx.background(0x000011ff);tx.image({resource,bounds:[0,0,240,135],clip:[0,0,240,135],sourceWidth:240,sourceHeight:135});})");CHECK(present()==KSN_OK);
 eval(ctx,"pocket.app={}");JSValue global=JS_GetGlobalObject(ctx),pk=JS_GetPropertyStr(ctx,global,"pocket"),app=JS_GetPropertyStr(ctx,pk,"app");CHECK(JS_SetPropertyStr(ctx,app,"load",JS_NewCFunction(ctx,load_generated_chunk,"load",1))>=0);JS_FreeValue(ctx,app);JS_FreeValue(ctx,pk);JS_FreeValue(ctx,global);bridge_file(ctx,"apps/skinasset/skin_asset_entry.js");char path[1024];CHECK(snprintf(path,sizeof path,"%s/arrays.js",argv[3])>0);bridge_file(ctx,path);bridge_file(ctx,"tools/ayatsuri/skin/generated_app_scenario.js");eval(ctx,"bridgeSetup()");
 CHECK(snprintf(path,sizeof path,"%s/expected.bin",argv[3])>0);FILE *f=fopen(path,"rb");CHECK(f);ayatsuri_skin_result expected[2][6];CHECK(fread(expected,1,sizeof expected,f)==sizeof expected&&fgetc(f)==EOF&&fclose(f)==0);
 size_t n;uint8_t *blob=load_fixture(argv[3],"plain",&n);ayatsuri_model model;CHECK(ayatsuri_model_open(blob,n,&model)==AYATSURI_MODEL_OK);size_t arena_bytes=ayatsuri_model_instance_bytes(&model);void *arena=malloc(arena_bytes);CHECK(arena);unsigned owner=0;ayatsuri_model_instance direct;CHECK(ayatsuri_model_instance_init(&model,arena,arena_bytes,&owner,1,&direct)==AYATSURI_MODEL_OK);
 JSValue evaluate=function(ctx,"bridgeEvaluate"),draw=function(ctx,"bridgeDraw");unsigned front[2]={0};uint32_t front_hash[2]={0};
 for(unsigned pose=0;pose<2;pose++)for(unsigned back=0;back<2;back++){
  call(ctx,evaluate,JS_NewInt32(ctx,(int)(pose*2+back)));
  ayatsuri_transform G[2],IB[2],palette[2];float V[36];uint16_t J[24],W[24];ayatsuri_skin_vertex input[6]={0};ayatsuri_skin_result result[6];
  copy_floats(ctx,"bridgeG",G,sizeof G);copy_floats(ctx,"bridgeIB",IB,sizeof IB);copy_floats(ctx,"bridgeV",V,sizeof V);copy_floats(ctx,"bridgeJ",J,sizeof J);copy_floats(ctx,"bridgeW",W,sizeof W);
  for(unsigned v=0;v<6;v++){memcpy(input[v].position,V+v*6,12);memcpy(input[v].normal,V+v*6+3,12);memcpy(input[v].joints,J+v*4,8);memcpy(input[v].weights,W+v*4,8);}
  CHECK(ayatsuri_skin_palette(G,IB,2,palette)==AYATSURI_OK&&ayatsuri_skin_evaluate(palette,2,input,6,result)==AYATSURI_OK);CHECK(geometry_close(result,expected[pose]));
  ayatsuri_skin_result bad[6];memcpy(bad,expected[pose],sizeof bad);bad[0].normal[2]=0;CHECK(!geometry_close(result,bad));bad[0]=expected[pose][0];bad[0].position[0]+=1;CHECK(!geometry_close(result,bad));
  const float camera[9]={back?-200:200,0,back?-400:400,0,0,0,1,120,67},light[6]={0,0,1,1,1,1};const ayatsuri_model_mesh_geometry geometry={0,6,expected[pose]};CHECK(ayatsuri_model_instance_evaluate_meshes(&direct,&owner,1,camera,light,&geometry,1)==AYATSURI_MODEL_OK);raster_oracle(&direct,&owner);
  call(ctx,draw,JS_UNDEFINED);CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));unsigned coverage=0;for(unsigned p=0;p<240*135;p++)coverage+=panel[p]!=17;
  CHECK(back?coverage==0:coverage>0);if(!back){front[pose]=coverage;uint32_t h=2166136261u;for(unsigned p=0;p<240*135;p++)h=(h^panel[p])*16777619u;front_hash[pose]=h;}
  printf("BLENDER_NATIVE_CASE pose=%u back=%u coverage=%u raw_position_normal_oracle=PASS complete_panel=PASS\n",pose,back,coverage);
 }
 CHECK(front_hash[0]!=front_hash[1]);
 call(ctx,evaluate,JS_NewInt32(ctx,0));size_t warm_calls=0;
 for(unsigned visit=0;visit<264;visit++){
  unsigned frame=visit<120?visit:visit<240?239-visit:90,pose=frame<60?0:1;
  size_t before=alloc_calls;call(ctx,evaluate,JS_NewInt32(ctx,(int)pose*2));warm_calls+=alloc_calls-before;call(ctx,draw,JS_UNDEFINED);
  if(visit==37){fail_send=true;CHECK(present()==KSN_IO&&pocket_proc_pending());}
  CHECK(present()==KSN_OK);uint32_t hash=2166136261u;for(unsigned p=0;p<240*135;p++)hash=(hash^panel[p])*16777619u;CHECK(hash==front_hash[pose]);
 }
 size_t guest_used,guest_limit;JS_GetMemoryCounters(rt,&guest_used,&guest_limit);
 printf("GENERATED_APP_RESOURCES evaluate264_alloc=%zu guest=%zu limit=%zu requested_live=%zu requested_peak=%zu skinWork=480\n",warm_calls,guest_used,guest_limit,live_bytes,peak_bytes);
 CHECK(!warm_calls);

 /* Submit front posed snapshot, then poison/release every source before present. */
 call(ctx,evaluate,JS_NewInt32(ctx,2));const float camera[9]={200,0,400,0,0,0,1,120,67},light[6]={0,0,1,1,1,1};const ayatsuri_model_mesh_geometry geometry={0,6,expected[1]};CHECK(ayatsuri_model_instance_evaluate_meshes(&direct,&owner,1,camera,light,&geometry,1)==AYATSURI_MODEL_OK);raster_oracle(&direct,&owner);
 /* Cold reference must differ from the posed app output. */
 CHECK(ayatsuri_model_instance_evaluate(&direct,&owner,1,camera,light)==AYATSURI_MODEL_OK);raster_oracle(&direct,&owner);call(ctx,draw,JS_UNDEFINED);CHECK(present()==KSN_OK&&memcmp(panel,reference,sizeof panel));
 CHECK(ayatsuri_model_instance_evaluate_meshes(&direct,&owner,1,camera,light,&geometry,1)==AYATSURI_MODEL_OK);raster_oracle(&direct,&owner);
 call(ctx,evaluate,JS_NewInt32(ctx,2));eval(ctx,"bridgePoisonInputs()");call(ctx,draw,JS_UNDEFINED);eval(ctx,"bridgePoisonDispose()");JS_RunGC(rt);CHECK(pocket_proc_pending()&&present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
 CHECK(ayatsuri_model_instance_dispose(&direct,&owner,1)==AYATSURI_MODEL_OK);free(arena);free(blob);JS_FreeValue(ctx,evaluate);JS_FreeValue(ctx,draw);pocket_shiki_api_reset();pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);shiki_heap_end();host_capabilities_clear();CHECK(!live_bytes&&!live_blocks);
 printf("GENERATED_APP_PASS rows=%u direct=%d front=%u/%u generated_mesh0 native_scalar_normal_oracle shared_renderer_panel oblique_pose_distinction cold_negative culling_front_back pending_poison_dispose release0\n",strip_rows,g_ksn_image_rows,front[0],front[1]);return 0;
}

int main(int argc,char **argv){for(unsigned cycle=0;cycle<3;cycle++){printf("GENERATED_APP_RUNTIME_CYCLE %u\n",cycle);CHECK(run_cycle(argc,argv)==0);}return 0;}
