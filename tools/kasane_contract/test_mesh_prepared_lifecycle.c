/* Keep the original suite unchanged and extend it for derived-state fallback. */
#include "original_depth_suite.inc"
int main(void){
 int result=original_mesh_depth_suite();if(result)return result;
 JSRuntime *rt=JS_NewRuntime();REQUIRE(rt);JSContext *ctx=JS_NewContext(rt);REQUIRE(ctx);
 JSValue g=JS_GetGlobalObject(ctx),p=JS_NewObject(ctx);REQUIRE(pocket_shiki_api_install(ctx,p)==ESP_OK);
 REQUIRE(JS_SetPropertyStr(ctx,g,"K",p)>=0);JS_FreeValue(ctx,g);eval_ok(ctx,"globalThis.P=K.shiki");
 ksn_proc_gouraud_patch red=triangle(63488);float z[3]={0,0,0};
 eval_ok(ctx,"P.beginMeshFrame(0,65536,8000000,1)");append(ctx,&red,z);
 pocket_proc_mesh_bytes before,after;pocket_proc_mesh_memory(&before);
 pocket_proc_mesh_fail_after(0);eval_ok(ctx,"P.commit()");pocket_proc_mesh_fail_after(-1);
 pocket_proc_mesh_memory(&after);REQUIRE(after.metadata==before.metadata);
 REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK&&actual[12*240+12]==63488);pocket_proc_present_result(KSN_OK);
 eval_ok(ctx,"P.beginMeshFrame(0,65536,8000000,1)");append(ctx,&red,z);eval_ok(ctx,"P.commit()");
 pocket_proc_mesh_memory(&after);REQUIRE(after.metadata>before.metadata);pocket_proc_present_result(KSN_OK);
 /* Warm the second retained slot before cap/growth failure paths. */
 eval_ok(ctx,"P.beginMeshFrame(0,65536,8000000,1)");append(ctx,&red,z);eval_ok(ctx,"P.commit()");pocket_proc_present_result(KSN_OK);
 eval_ok(ctx,"P.beginMeshFrame(0,65536,8000000,1)");for(unsigned j=0;j<64;j++)append(ctx,&red,z);
 ksn_proc_gouraud_patch blue=triangle(31);z[0]=z[1]=z[2]=1;append(ctx,&blue,z);
 pocket_proc_mesh_memory(&before);eval_ok(ctx,"P.commit()");pocket_proc_mesh_memory(&after);REQUIRE(after.metadata==before.metadata);
 REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK&&actual[12*240+12]==31);pocket_proc_present_result(KSN_OK);
 /* Both slots now exist: fail replacement growth, never render stale setup. */
 eval_ok(ctx,"P.beginMeshFrame(0,65536,8000000,1)");append(ctx,&red,z);
 z[0]=z[1]=z[2]=2;append(ctx,&blue,z);
 pocket_proc_mesh_memory(&before);pocket_proc_mesh_fail_after(0);eval_ok(ctx,"P.commit()");pocket_proc_mesh_fail_after(-1);
 pocket_proc_mesh_memory(&after);REQUIRE(after.metadata==before.metadata);
 REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK&&actual[12*240+12]==31);pocket_proc_present_result(KSN_OK);
 pocket_proc_reset();pocket_proc_mesh_memory(&after);REQUIRE(!after.candidate&&!after.committed&&!after.workspace);
 JS_FreeContext(ctx);JS_FreeRuntime(rt);
 puts("PREPARED_LIFECYCLE_PASS original_suite OOM_fallback next_frame_prepared visible_face65 growth_OOM_fallback no_dropped_faces reset");return 0;
}
