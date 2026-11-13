#define SKIN_JS_MAIN retained_main
#include "../ayatsuri/skin/test_motion.c"
#undef SKIN_JS_MAIN
static uint16_t images[4][240*135];
int main(int argc,char **argv){
 CHECK(argc==4);strip_rows=(unsigned)atoi(argv[1]);g_ksn_image_rows=atoi(argv[2]);delayed=true;
 JSRuntime *rt=JS_NewRuntime();CHECK(rt);JSContext *ctx=JS_NewContext(rt);CHECK(ctx&&JS_PocketHarden(ctx)==0);JS_SetMemoryLimit(rt,160*1024);
 host_capabilities_clear();CHECK(shiki_heap_begin(0));CHECK(pocket_kasane_install(ctx,NULL)==ESP_OK);CHECK(pocket_shape_install(ctx,NULL)==ESP_OK);
 expose_fixture(ctx,argv[3],"plain","fixture");
 eval(ctx,"globalThis.pocket={kasane,ayatsuri};globalThis.P=kasane.shiki;const res=P.resource();kasane.replace(tx=>{tx.background(0x000011ff);tx.image({resource:res,bounds:[0,0,240,135],clip:[0,0,240,135],sourceWidth:240,sourceHeight:135});})");CHECK(present()==KSN_OK);
 motion_file(ctx,"apps/lib/ayatsuri_model.js");motion_file(ctx,"tools/blink_bridge/scenario.js");JSValue step=function(ctx,"step");
 const double values[]={0,.5,.979999,.97999999,.98,.980001,1};
 for(unsigned k=0;k<686;k++){unsigned n=k<343?k:685-k;double b=values[n/49],l=values[n/7%7],r=values[n%7];call(ctx,step,JS_NewInt32(ctx,(int)n));if(k==37){eval(ctx,"invalid()");CHECK(pocket_proc_pending());fail_send=true;CHECK(present()==KSN_IO&&pocket_proc_pending());}CHECK(present()==KSN_OK);
  const double levels[2]={fmax(b,l),fmax(b,r)};unsigned covered[2]={0,0};
  for(unsigned y=0;y<135;y++)for(unsigned x=0;x<240;x++){bool inside=false;for(unsigned s=0;s<2;s++){double cx=s?180:60,half=(float)(1-levels[s]);bool hit=levels[s]<.98&&x>=cx-20&&x<cx+20&&y>=(float)(67-20*half)&&y<(float)(67+20*half);inside|=hit;if(hit)covered[s]++;}if((panel[y*240+x]!=17)!=inside){fprintf(stderr,"MISMATCH n=%u x=%u y=%u pixel=%u expected=%d\n",n,x,y,panel[y*240+x],inside);CHECK(false);}}
  if(n==0){CHECK(covered[0]==1600&&covered[1]==1600);memcpy(images[0],panel,sizeof panel);}
 }
 eval(ctx,"invalid();getters()");CHECK(present()==KSN_OK&&!memcmp(panel,images[0],sizeof panel));call(ctx,step,JS_NewInt32(ctx,0));eval(ctx,"stopPending()");JS_RunGC(rt);CHECK(pocket_proc_pending()&&present()==KSN_OK&&!memcmp(panel,images[0],sizeof panel));
 JS_FreeValue(ctx,step);pocket_shiki_api_reset();pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);shiki_heap_end();host_capabilities_clear();CHECK(!live_bytes&&!live_blocks);
 printf("BLINK_BRIDGE_PASS rows=%u direct=%d combinations343 reverse343 analytic_integer_top_left IO_retry pending_poison_dispose ledger0 sparse_getters_invalid_pending_atomic identity_skin_geometry_only\n",strip_rows,g_ksn_image_rows);return 0;
}
