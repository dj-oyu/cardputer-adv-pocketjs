#define SKIN_JS_MAIN retained_skin_main
#include "../skin/test_motion.c"
#undef SKIN_JS_MAIN
static uint16_t saved[240*135];
static float truth[4][2][24];
static void expected(ayatsuri_model_instance *a,const void *owner,unsigned n){
 ayatsuri_skin_result v[2][4];ayatsuri_model_mesh_geometry g[2];for(unsigned p=0;p<2;p++){for(unsigned k=0;k<4;k++){memcpy(v[p][k].position,truth[n][p]+6*k,12);memcpy(v[p][k].normal,truth[n][p]+6*k+3,12);}g[p]=(ayatsuri_model_mesh_geometry){(uint16_t)(p+1),4,v[p]};}
 const float camera[9]={0,0,400,0,0,0,20,120,67},light[6]={0,0,1,1,1,1};CHECK(ayatsuri_model_instance_evaluate_meshes(a,owner,1,camera,light,g,2)==AYATSURI_MODEL_OK);raster_oracle(a,owner);
}
int main(int argc,char **argv){
 CHECK(argc==4);strip_rows=(unsigned)atoi(argv[1]);g_ksn_image_rows=atoi(argv[2]);delayed=true;
 JSRuntime *rt=JS_NewRuntime();CHECK(rt);JSContext *ctx=JS_NewContext(rt);CHECK(ctx&&JS_PocketHarden(ctx)==0);JS_SetMemoryLimit(rt,160*1024);
 host_capabilities_clear();CHECK(shiki_heap_begin(0));CHECK(pocket_kasane_install(ctx,NULL)==ESP_OK);CHECK(pocket_shape_install(ctx,NULL)==ESP_OK);expose_fixture(ctx,argv[3],"plain","fixture");
 eval(ctx,"globalThis.pocket={kasane,ayatsuri};globalThis.P=kasane.shiki;const resource=P.resource();kasane.replace(tx=>{tx.background(0x000011ff);tx.image({resource,bounds:[0,0,240,135],clip:[0,0,240,135],sourceWidth:240,sourceHeight:135});})");CHECK(present()==KSN_OK);
 char path[1024];snprintf(path,sizeof path,"%s/expected.bin",argv[3]);FILE *f=fopen(path,"rb");CHECK(f&&fread(truth,1,sizeof truth,f)==sizeof truth&&fgetc(f)==EOF&&fclose(f)==0);snprintf(path,sizeof path,"%s/cases.js",argv[3]);motion_file(ctx,path);motion_file(ctx,"apps/lib/ayatsuri_model.js");motion_file(ctx,"tools/ayatsuri/dq_multipart/scenario.js");
 size_t blob_bytes;uint8_t *blob=load_fixture(argv[3],"plain",&blob_bytes);ayatsuri_model model;CHECK(ayatsuri_model_open(blob,blob_bytes,&model)==AYATSURI_MODEL_OK);size_t bytes=ayatsuri_model_instance_bytes(&model);void *arena=malloc(bytes);CHECK(arena);unsigned owner=0;ayatsuri_model_instance direct;CHECK(ayatsuri_model_instance_init(&model,arena,bytes,&owner,1,&direct)==AYATSURI_MODEL_OK);
 JSValue step=function(ctx,"step"),evaluate=function(ctx,"evaluate");
 for(unsigned visit=0;visit<120;visit++){unsigned n=visit<60?visit%4:3-visit%4;call(ctx,step,JS_NewInt32(ctx,n));expected(&direct,&owner,n);CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));}
 eval(ctx,"invalid()");
 const float camera[9]={0,0,400,0,0,0,20,120,67},light[6]={0,0,1,1,1,1};CHECK(ayatsuri_model_instance_evaluate(&direct,&owner,1,camera,light)==AYATSURI_MODEL_OK);raster_oracle(&direct,&owner);eval(ctx,"empty()");CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
 call(ctx,step,JS_NewInt32(ctx,0));expected(&direct,&owner,0);eval(ctx,"projectFail()");CHECK(pocket_proc_pending()&&present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
 eval(ctx,"fresh()");JS_RunGC(rt);size_t used,limit;JS_GetMemoryCounters(rt,&used,&limit);JS_SetMemoryLimit(rt,used);JSValue ret=JS_Call(ctx,evaluate,JS_UNDEFINED,0,NULL);JS_SetMemoryLimit(rt,160*1024);CHECK(JS_IsException(ret));JSValue e=JS_GetException(ctx);JS_FreeValue(ctx,e);JS_FreeValue(ctx,ret);eval(ctx,"failed(true);warmSmall()");CHECK(present()==KSN_OK);memcpy(saved,panel,sizeof saved);eval(ctx,"warmSmall()");JS_RunGC(rt);JS_GetMemoryCounters(rt,&used,&limit);JS_SetMemoryLimit(rt,used);
 ret=JS_Call(ctx,evaluate,JS_UNDEFINED,0,NULL);JS_SetMemoryLimit(rt,160*1024);CHECK(JS_IsException(ret));e=JS_GetException(ctx);JS_FreeValue(ctx,e);JS_FreeValue(ctx,ret);eval(ctx,"failed(false)");CHECK(pocket_proc_pending()&&present()==KSN_OK&&!memcmp(panel,saved,sizeof panel));
 call(ctx,evaluate,JS_UNDEFINED);eval(ctx,"grown()");expected(&direct,&owner,0);eval(ctx,"poison()");JS_RunGC(rt);CHECK(pocket_proc_pending()&&present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
 CHECK(ayatsuri_model_instance_dispose(&direct,&owner,1)==AYATSURI_MODEL_OK);free(arena);free(blob);JS_FreeValue(ctx,step);JS_FreeValue(ctx,evaluate);pocket_shiki_api_reset();pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);shiki_heap_end();host_capabilities_clear();CHECK(!live_bytes&&!live_blocks);
 printf("SCENE_MULTIPART_DQ_PASS rows=%u direct=%d cases4 visits120 descriptor_reorder oracle_pixels secondpart_invalid_no_core_evals empty_cold time_busy initial_grow_OOM pending_projection_poison_disposal ledger0\n",strip_rows,g_ksn_image_rows);return 0;
}
