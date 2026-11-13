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
 motion_file(ctx,"apps/lib/ayatsuri_model.js");motion_file(ctx,"tools/blink_visibility/scenario.js");JSValue flag=function(ctx,"flags"),step=function(ctx,"step");
 for(int i=0;i<4;i++){call(ctx,flag,JS_NewInt32(ctx,i));CHECK(present()==KSN_OK);memcpy(images[i],panel,sizeof panel);}
 unsigned left=0,right=0;for(unsigned i=0;i<240*135;i++){CHECK(images[0][i]==17);bool l=images[1][i]!=17,r=images[2][i]!=17;CHECK(!(l&&r));if(l){left++;CHECK(i%240<120);}if(r){right++;CHECK(i%240>=120);}CHECK(images[3][i]==(l?images[1][i]:images[2][i]));}CHECK(left>20&&right>20);
 const double values[]={0,.5,.979999,.97999999,.98,.980001,1};
 for(unsigned k=0;k<686;k++){unsigned n=k<343?k:685-k;double b=values[n/49],l=values[n/7%7],r=values[n%7];unsigned mask=((b<.98&&l<.98)?1:0)|((b<.98&&r<.98)?2:0);call(ctx,step,JS_NewInt32(ctx,(int)n));if(k==37){fail_send=true;CHECK(present()==KSN_IO&&pocket_proc_pending());}CHECK(present()==KSN_OK&&!memcmp(panel,images[mask],sizeof panel));}
 eval(ctx,"invalid()");call(ctx,flag,JS_NewInt32(ctx,3));eval(ctx,"stopPending()");JS_RunGC(rt);CHECK(pocket_proc_pending()&&present()==KSN_OK&&!memcmp(panel,images[3],sizeof panel));
 JS_FreeValue(ctx,flag);JS_FreeValue(ctx,step);pocket_shiki_api_reset();pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);shiki_heap_end();host_capabilities_clear();CHECK(!live_bytes&&!live_blocks);
 printf("BLINK_VISIBILITY_PASS rows=%u direct=%d combinations343 reverse343 left=%u right=%u invalid IO_retry pending_mutation_dispose ledger0 visibility_only\n",strip_rows,g_ksn_image_rows,left,right);return 0;
}
