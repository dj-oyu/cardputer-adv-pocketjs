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
int main(int argc,char **argv){
 CHECK(argc==4);strip_rows=(unsigned)atoi(argv[1]);g_ksn_image_rows=atoi(argv[2]);delayed=true;
 JSRuntime *rt=JS_NewRuntime();CHECK(rt);JSContext *ctx=JS_NewContext(rt);CHECK(ctx&&JS_PocketHarden(ctx)==0);JS_SetMemoryLimit(rt,160*1024);
 host_capabilities_clear();CHECK(shiki_heap_begin(0));CHECK(pocket_kasane_install(ctx,NULL)==ESP_OK);CHECK(pocket_shape_install(ctx,NULL)==ESP_OK);
 expose_fixture(ctx,argv[3],"plain","fixture");
 eval(ctx,"globalThis.pocket={kasane,ayatsuri};globalThis.P=kasane.shiki;const resource=P.resource();kasane.replace(tx=>{tx.background(0x000011ff);tx.image({resource,bounds:[0,0,240,135],clip:[0,0,240,135],sourceWidth:240,sourceHeight:135});})");CHECK(present()==KSN_OK);
 bridge_file(ctx,"apps/lib/ayatsuri_model.js");char path[1024];CHECK(snprintf(path,sizeof path,"%s/arrays.js",argv[3])>0);bridge_file(ctx,path);bridge_file(ctx,"tools/ayatsuri/skin/blender_rotation_native_scenario.js");eval(ctx,"bridgeSetup()");
 CHECK(snprintf(path,sizeof path,"%s/expected.bin",argv[3])>0);FILE *f=fopen(path,"rb");CHECK(f);ayatsuri_skin_result expected[3][6];CHECK(fread(expected,1,sizeof expected,f)==sizeof expected&&fgetc(f)==EOF&&fclose(f)==0);
 size_t n;uint8_t *blob=load_fixture(argv[3],"plain",&n);ayatsuri_model model;CHECK(ayatsuri_model_open(blob,n,&model)==AYATSURI_MODEL_OK);size_t arena_bytes=ayatsuri_model_instance_bytes(&model);void *arena=malloc(arena_bytes);CHECK(arena);unsigned owner=0;ayatsuri_model_instance direct;CHECK(ayatsuri_model_instance_init(&model,arena,arena_bytes,&owner,1,&direct)==AYATSURI_MODEL_OK);
 JSValue evaluate=function(ctx,"bridgeEvaluate"),draw=function(ctx,"bridgeDraw");unsigned front[3]={0};uint32_t front_hash[3]={0};
 for(unsigned pose=0;pose<3;pose++)for(unsigned back=0;back<2;back++){
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
 CHECK(front_hash[0]!=front_hash[1]&&front_hash[0]!=front_hash[2]);
 /* Submit front posed snapshot, then poison/release every source before present. */
 call(ctx,evaluate,JS_NewInt32(ctx,4));const float camera[9]={200,0,400,0,0,0,1,120,67},light[6]={0,0,1,1,1,1};const ayatsuri_model_mesh_geometry geometry={0,6,expected[2]};CHECK(ayatsuri_model_instance_evaluate_meshes(&direct,&owner,1,camera,light,&geometry,1)==AYATSURI_MODEL_OK);raster_oracle(&direct,&owner);
 eval(ctx,"bridgeCold()");call(ctx,draw,JS_UNDEFINED);CHECK(present()==KSN_OK&&memcmp(panel,reference,sizeof panel));
 call(ctx,evaluate,JS_NewInt32(ctx,4));eval(ctx,"bridgePoisonInputs()");call(ctx,draw,JS_UNDEFINED);eval(ctx,"bridgePoisonDispose()");JS_RunGC(rt);CHECK(pocket_proc_pending()&&present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
 CHECK(ayatsuri_model_instance_dispose(&direct,&owner,1)==AYATSURI_MODEL_OK);free(arena);free(blob);JS_FreeValue(ctx,evaluate);JS_FreeValue(ctx,draw);pocket_shiki_api_reset();pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);shiki_heap_end();host_capabilities_clear();CHECK(!live_bytes&&!live_blocks);
 printf("BLENDER_NATIVE_PASS rows=%u direct=%d front=%u/%u generated_mesh0 native_scalar_normal_oracle shared_renderer_panel oblique_pose_distinction cold_negative culling_front_back pending_poison_dispose release0\n",strip_rows,g_ksn_image_rows,front[0],front[1]);return 0;
}
