/* Q7 derived state follows the post-131 single front frame, not ACK swaps. */
#include "original_q7_helpers.inc"
#include "ui/kasane/ksn_mesh_q7_certified.h"
extern const ksn_mesh_q7_snapshot *pocket_proc_q7_host_snapshot(unsigned,bool);
static const ksn_mesh_q7_snapshot *front(void){return pocket_proc_q7_host_snapshot(0,true);}
static void frame(JSContext *ctx,ksn_proc_gouraud_patch *p,float *z,unsigned count){
 eval_ok(ctx,"P.beginMeshFrame(0,65536,8000000,1)");
 for(unsigned i=0;i<count;i++)append(ctx,p,z);
 eval_ok(ctx,"P.commit()");
}
static void pixel(uint16_t color){REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK&&actual[2892]==color);}
int main(void){
 /* Helpers only: Q7 coverage is approximate; the original full pixel suite
  * runs separately with Q7 off, not as a universal Q7 visual oracle. */
 JSRuntime*rt=JS_NewRuntime();REQUIRE(rt);JSContext*ctx=JS_NewContext(rt);REQUIRE(ctx);
 JSValue g=JS_GetGlobalObject(ctx),p=JS_NewObject(ctx);REQUIRE(pocket_shiki_api_install(ctx,p)==ESP_OK);
 REQUIRE(JS_SetPropertyStr(ctx,g,"K",p)>=0);JS_FreeValue(ctx,g);
 eval_ok(ctx,"globalThis.P=K.shiki;globalThis.pocket={kasane:K}");
 ksn_proc_gouraud_patch red=triangle(63488),blue=triangle(31);float z[3]={0,0,0};
 /* Fail prepared, shared owner, and Q7 allocation on a fresh surface. */
 for(int fail=0;fail<3;fail++){
  pocket_proc_reset();eval_ok(ctx,"P.beginMeshFrame(0,65536,8000000,1)");append(ctx,&red,z);
  pocket_proc_mesh_fail_after(fail);eval_ok(ctx,"P.commit()");pocket_proc_mesh_fail_after(-1);
  REQUIRE(!front()||!front()->valid);pixel(63488);pocket_proc_present_result(KSN_OK);
 }
 /* Commit, not ACK, makes the Q7 candidate the front. ACK preserves it. */
 pocket_proc_reset();frame(ctx,&red,z,1);const ksn_mesh_q7_snapshot *red_front=front();
 REQUIRE(red_front&&red_front->valid&&red_front->count==1);pixel(63488);
 pocket_proc_present_result(KSN_OK);REQUIRE(front()==red_front);
 /* Invalid admission and a built-but-uncommitted candidate cannot change front. */
 size_t red_bytes=sizeof(*red_front)+red_front->count*sizeof red_front->triangles[0];
 unsigned char *red_saved=malloc(red_bytes);REQUIRE(red_saved);memcpy(red_saved,red_front,red_bytes);
 eval_error(ctx,"P.beginMeshFrame(0,65536,8000000,2)");
 REQUIRE(front()==red_front&&!memcmp(red_saved,red_front,red_bytes));pixel(63488);
 eval_ok(ctx,"P.beginMeshFrame(0,65536,8000000,1)");append(ctx,&blue,z);
 REQUIRE(front()==red_front&&!memcmp(red_saved,red_front,red_bytes));pixel(63488);
 free(red_saved);eval_ok(ctx,"P.commit()");const ksn_mesh_q7_snapshot *blue_front=front();
 REQUIRE(blue_front&&blue_front->valid&&blue_front!=red_front);pixel(31);
 /* Backdrop IO repairs the NEW front, as the old frame was overwritten. */
 pocket_proc_present_result(KSN_IO);REQUIRE(!pocket_proc_pending());pixel(31);
 REQUIRE(front()==blue_front);eval_error(ctx,"P.beginFrame(0)");
 pocket_proc_present_result(KSN_OK);REQUIRE(front()==blue_front);
 /* Both old Q7 slots exist. Preparation OOM must not reuse stale red. */
 eval_ok(ctx,"P.beginMeshFrame(0,65536,8000000,1)");append(ctx,&blue,z);
 pocket_proc_mesh_fail_after(0);eval_ok(ctx,"P.commit()");pocket_proc_mesh_fail_after(-1);
 REQUIRE(front()&&!front()->valid);pixel(31);pocket_proc_present_result(KSN_IO);pixel(31);
 pocket_proc_present_result(KSN_OK);
 /* Count 64 admitted; face 65 must render by fallback, not stale Q7. */
 frame(ctx,&red,z,64);REQUIRE(front()&&front()->valid&&front()->count==64);pocket_proc_present_result(KSN_OK);
 eval_ok(ctx,"P.beginMeshFrame(0,65536,8000000,1)");for(unsigned i=0;i<64;i++)append(ctx,&red,z);
 z[0]=z[1]=z[2]=1;append(ctx,&blue,z);eval_ok(ctx,"P.commit()");
 REQUIRE(!front()||!front()->valid);pixel(31);pocket_proc_present_result(KSN_IO);pixel(31);pocket_proc_present_result(KSN_OK);
 /* Image IO retains the identical submitted front through retry and ACK. */
 pocket_proc_image_mode();frame(ctx,&blue,z,1);const ksn_mesh_q7_snapshot *submitted=front();
 REQUIRE(submitted&&submitted->valid&&submitted->count==1);
 size_t bytes=sizeof(*submitted)+submitted->count*sizeof submitted->triangles[0];
 unsigned char *saved=malloc(bytes);REQUIRE(saved);memcpy(saved,submitted,bytes);
 pocket_proc_present_result(KSN_IO);REQUIRE(pocket_proc_pending()&&front()==submitted);
 eval_error(ctx,"P.beginFrame(0)");REQUIRE(!memcmp(saved,submitted,bytes));
 ksn_image_port port;pocket_proc_image_port(&port);REQUIRE(pocket_proc_backdrop(NULL,0,135,expected)==KSN_OK);
 for(unsigned y=0;y<135;y+=4){unsigned n=135-y;if(n>4)n=4;REQUIRE(port.read_rows(port.ctx,0,0,y,n,0,240,actual+y*240,240)==KSN_OK);}
 REQUIRE(!memcmp(actual,expected,sizeof actual));pocket_proc_present_result(KSN_OK);
 REQUIRE(front()==submitted&&!pocket_proc_pending()&&!memcmp(saved,submitted,bytes));free(saved);
 pocket_proc_mesh_bytes memory;pocket_proc_mesh_memory(&memory);
 printf("Q7_LIFECYCLE_PASS single_buffer initial_OOM3 commit_front ACK_no_swap stale_Q7_rejected backdrop_newfront_repair count64 face65_fallback image_retry_ACK metadata=%zu workspace=%zu\n",memory.metadata,memory.workspace);
 pocket_proc_reset();pocket_proc_mesh_memory(&memory);REQUIRE(!memory.candidate&&!memory.committed&&!memory.workspace);
 REQUIRE(!front()&&!pocket_proc_q7_host_snapshot(0,false));
 frame(ctx,&red,z,1);REQUIRE(front()&&front()->valid);pixel(63488);pocket_proc_present_result(KSN_OK);pocket_proc_reset();
 JS_FreeContext(ctx);JS_FreeRuntime(rt);return 0;
}
