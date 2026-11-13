#define SKIN_JS_MAIN retained_skin_main
#include "../skin/test_motion.c"
#undef SKIN_JS_MAIN
static bool watching;static ayatsuri_skin_result captured[4];static unsigned captures;
extern ayatsuri_model_status __real_ayatsuri_model_instance_evaluate_meshes(ayatsuri_model_instance *,const void *,uint32_t,const float[9],const float[6],const ayatsuri_model_mesh_geometry *,size_t);
ayatsuri_model_status __wrap_ayatsuri_model_instance_evaluate_meshes(ayatsuri_model_instance *i,const void *o,uint32_t g,const float c[9],const float l[6],const ayatsuri_model_mesh_geometry *m,size_t n){
 if(watching){CHECK(n==1&&m[0].node_id==1&&m[0].vertex_count==4);memcpy(captured,m[0].vertices,sizeof captured);captures++;}
 return __real_ayatsuri_model_instance_evaluate_meshes(i,o,g,c,l,m,n);
}
static uint16_t saved[240*135];static float truth[6][24];
static void expected(ayatsuri_model_instance *a,const void *owner,unsigned n){
 ayatsuri_skin_result v[4];for(unsigned k=0;k<4;k++){memcpy(v[k].position,truth[n]+6*k,12);memcpy(v[k].normal,truth[n]+6*k+3,12);}
 const float camera[9]={0,0,400,0,0,0,20,120,67},light[6]={0,0,1,1,1,1};const ayatsuri_model_mesh_geometry g={1,4,v};CHECK(ayatsuri_model_instance_evaluate_meshes(a,owner,1,camera,light,&g,1)==AYATSURI_MODEL_OK);raster_oracle(a,owner);
}
int main(int argc,char **argv){
 CHECK(argc==4);strip_rows=(unsigned)atoi(argv[1]);g_ksn_image_rows=atoi(argv[2]);delayed=true;
 char path[1024];snprintf(path,sizeof path,"%s/expected.bin",argv[3]);FILE *f=fopen(path,"rb");CHECK(f&&fread(truth,1,sizeof truth,f)==sizeof truth&&fgetc(f)==EOF&&fclose(f)==0);
 JSRuntime *rt=JS_NewRuntime();CHECK(rt);JSContext *ctx=JS_NewContext(rt);CHECK(ctx&&JS_PocketHarden(ctx)==0);JS_SetMemoryLimit(rt,160*1024);
 host_capabilities_clear();CHECK(shiki_heap_begin(0));CHECK(pocket_kasane_install(ctx,NULL)==ESP_OK);CHECK(pocket_shape_install(ctx,NULL)==ESP_OK);expose_fixture(ctx,argv[3],"plain","fixture");
 eval(ctx,"globalThis.pocket={kasane,ayatsuri};globalThis.P=kasane.shiki;const resource=P.resource();kasane.replace(tx=>{tx.background(0x000011ff);tx.image({resource,bounds:[0,0,240,135],clip:[0,0,240,135],sourceWidth:240,sourceHeight:135});})");CHECK(present()==KSN_OK);
 snprintf(path,sizeof path,"%s/cases.js",argv[3]);motion_file(ctx,path);motion_file(ctx,"apps/lib/ayatsuri_model.js");motion_file(ctx,"tools/ayatsuri/dq_mixed/scenario.js");
 size_t nb;uint8_t *blob=load_fixture(argv[3],"plain",&nb);ayatsuri_model model;CHECK(ayatsuri_model_open(blob,nb,&model)==AYATSURI_MODEL_OK);size_t bytes=ayatsuri_model_instance_bytes(&model);void *arena=malloc(bytes);CHECK(arena);unsigned owner=0;ayatsuri_model_instance direct;CHECK(ayatsuri_model_instance_init(&model,arena,bytes,&owner,1,&direct)==AYATSURI_MODEL_OK);
 uint8_t dirty[3]={0,0,AYATSURI_TARGET_HIDDEN};float poses[27]={0};uint32_t colors[3]={0};double opacity[3]={0};const ayatsuri_model_targets targets={dirty,poses,colors,opacity,3};CHECK(ayatsuri_model_instance_targets(&direct,&owner,1,&targets)==AYATSURI_MODEL_OK);
 JSValue step=function(ctx,"step"),grow=function(ctx,"grow");double maxpos=0,maxnormal=0;
 for(unsigned visit=0;visit<120;visit++){unsigned n=visit<60?visit%6:5-visit%6;captures=0;watching=true;call(ctx,step,JS_NewInt32(ctx,n));watching=false;CHECK(captures==1);
  for(unsigned k=0;k<4;k++)for(unsigned c=0;c<3;c++){double dp=fabs(captured[k].position[c]-truth[n][6*k+c]),dn=fabs(captured[k].normal[c]-truth[n][6*k+3+c]);if(dp>maxpos)maxpos=dp;if(dn>maxnormal)maxnormal=dn;CHECK(dp<=2e-6&&dn<=2e-6);}
  expected(&direct,&owner,n);CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
 }
 eval(ctx,"fresh();warm()");CHECK(present()==KSN_OK);memcpy(saved,panel,sizeof saved);eval(ctx,"warm()");CHECK(pocket_proc_pending());JS_RunGC(rt);size_t used,limit;JS_GetMemoryCounters(rt,&used,&limit);JS_SetMemoryLimit(rt,used);
 JSValue ret=JS_Call(ctx,grow,JS_UNDEFINED,0,NULL);JS_SetMemoryLimit(rt,160*1024);CHECK(JS_IsException(ret));JSValue e=JS_GetException(ctx);JS_FreeValue(ctx,e);JS_FreeValue(ctx,ret);eval(ctx,"growthFailed()");CHECK(pocket_proc_pending()&&present()==KSN_OK&&!memcmp(panel,saved,sizeof panel));
 call(ctx,grow,JS_UNDEFINED);eval(ctx,"grown()");expected(&direct,&owner,0);eval(ctx,"poison()");JS_RunGC(rt);CHECK(pocket_proc_pending()&&present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
 CHECK(ayatsuri_model_instance_dispose(&direct,&owner,1)==AYATSURI_MODEL_OK);free(arena);free(blob);JS_FreeValue(ctx,step);JS_FreeValue(ctx,grow);pocket_shiki_api_reset();pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);shiki_heap_end();host_capabilities_clear();CHECK(!live_bytes&&!live_blocks);
 printf("SCENE_MIXED_DQ_PASS rows=%u direct=%d cases6 visits120 raw_position_max=%.9g raw_normal_max=%.9g oracle_pixels warm_grow304_352_OOM_old_pending_recovery poison_disposal ledger0\n",strip_rows,g_ksn_image_rows,maxpos,maxnormal);return 0;
}
