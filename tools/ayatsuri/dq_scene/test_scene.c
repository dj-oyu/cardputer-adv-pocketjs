#define SKIN_JS_MAIN retained_skin_main
#include "../skin/test_motion.c"
#undef SKIN_JS_MAIN
static uint16_t saved[240*135];
static void expected(ayatsuri_model_instance *a,const void *owner,unsigned n){
 const unsigned side=n/6,level=n/2%3;const double blinks[3]={0,.5,.97999999};const double half=(float)(1-blinks[level]),co=sqrt(3)/2,si=n%2?.5:-.5;
 ayatsuri_skin_result v[4];const double cx=side?3:-3;const int sx[4]={-1,1,1,-1},sy[4]={-1,-1,1,1};
 for(unsigned k=0;k<4;k++){double y=(float)(half*sy[k]);v[k]=(ayatsuri_skin_result){{(float)(cx+sx[k]+2),(float)(co*y-1),(float)(si*y+3)},{0,(float)-si,(float)co}};}
 const float camera[9]={0,0,400,0,0,0,20,120,67},light[6]={0,0,1,1,1,1};const ayatsuri_model_mesh_geometry g={1,4,v};CHECK(ayatsuri_model_instance_evaluate_meshes(a,owner,1,camera,light,&g,1)==AYATSURI_MODEL_OK);raster_oracle(a,owner);
}
int main(int argc,char **argv){
 CHECK(argc==4);strip_rows=(unsigned)atoi(argv[1]);g_ksn_image_rows=atoi(argv[2]);delayed=true;
 JSRuntime *rt=JS_NewRuntime();CHECK(rt);JSContext *ctx=JS_NewContext(rt);CHECK(ctx&&JS_PocketHarden(ctx)==0);JS_SetMemoryLimit(rt,160*1024);
 host_capabilities_clear();CHECK(shiki_heap_begin(0));CHECK(pocket_kasane_install(ctx,NULL)==ESP_OK);CHECK(pocket_shape_install(ctx,NULL)==ESP_OK);expose_fixture(ctx,argv[3],"plain","fixture");
 eval(ctx,"globalThis.pocket={kasane,ayatsuri};globalThis.P=kasane.shiki;const resource=P.resource();kasane.replace(tx=>{tx.background(0x000011ff);tx.image({resource,bounds:[0,0,240,135],clip:[0,0,240,135],sourceWidth:240,sourceHeight:135});})");CHECK(present()==KSN_OK);
 char path[1024];snprintf(path,sizeof path,"%s/cases.js",argv[3]);motion_file(ctx,path);motion_file(ctx,"apps/lib/ayatsuri_model.js");motion_file(ctx,"tools/ayatsuri/dq_scene/scenario.js");
 size_t blob_bytes;uint8_t *blob=load_fixture(argv[3],"plain",&blob_bytes);ayatsuri_model model;CHECK(ayatsuri_model_open(blob,blob_bytes,&model)==AYATSURI_MODEL_OK);size_t bytes=ayatsuri_model_instance_bytes(&model);void *arena=malloc(bytes);CHECK(arena);unsigned owner=0;ayatsuri_model_instance direct;CHECK(ayatsuri_model_instance_init(&model,arena,bytes,&owner,1,&direct)==AYATSURI_MODEL_OK);
 uint8_t dirty[3]={0,0,AYATSURI_TARGET_HIDDEN};float poses[27]={0};uint32_t colors[3]={0};double opacity[3]={0};const ayatsuri_model_targets targets={dirty,poses,colors,opacity,3};CHECK(ayatsuri_model_instance_targets(&direct,&owner,1,&targets)==AYATSURI_MODEL_OK);
 JSValue step=function(ctx,"step"),evaluate=function(ctx,"evaluateOnly");
 for(unsigned visit=0;visit<240;visit++){unsigned n=visit<120?visit%12:11-visit%12;call(ctx,step,JS_NewInt32(ctx,n));expected(&direct,&owner,n);if(visit==37){fail_send=true;CHECK(present()==KSN_IO&&pocket_proc_pending());}CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));}
 call(ctx,step,JS_NewInt32(ctx,0));expected(&direct,&owner,0);eval(ctx,"pendingFailure()");CHECK(pocket_proc_pending()&&present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
 eval(ctx,"bad();fresh()");JS_RunGC(rt);size_t used,limit;JS_GetMemoryCounters(rt,&used,&limit);JS_SetMemoryLimit(rt,used);JSValue arg=JS_NewInt32(ctx,0),ret=JS_Call(ctx,evaluate,JS_UNDEFINED,1,&arg);JS_SetMemoryLimit(rt,160*1024);CHECK(JS_IsException(ret));JSValue e=JS_GetException(ctx);JS_FreeValue(ctx,e);JS_FreeValue(ctx,ret);eval(ctx,"failed()");
 call(ctx,step,arg);expected(&direct,&owner,0);memcpy(saved,reference,sizeof saved);eval(ctx,"poison()");JS_RunGC(rt);CHECK(pocket_proc_pending()&&present()==KSN_OK&&!memcmp(panel,saved,sizeof panel));
 CHECK(ayatsuri_model_instance_dispose(&direct,&owner,1)==AYATSURI_MODEL_OK);free(arena);free(blob);JS_FreeValue(ctx,step);JS_FreeValue(ctx,evaluate);pocket_shiki_api_reset();pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);shiki_heap_end();host_capabilities_clear();CHECK(!live_bytes&&!live_blocks);
 printf("SCENE_RIGID_DQ_PASS rows=%u direct=%d cases12 visits240 oracle_pixels invalid_rigid_weight_time busy_oom_recovery pending_poison_disposal ledger0\n",strip_rows,g_ksn_image_rows);return 0;
}
