#define pocket_api_lazy mesh_unused_lazy
#define main baseline_fixture_main
#include "test_pocket_proc_qjs.c"
#undef main
#undef pocket_api_lazy
#include "ui/kasane/ksn_mesh_depth.h"
#include <math.h>
void pocket_proc_host_classes_reset(void);
#include <time.h>
esp_err_t pocket_api_lazy(JSContext *ctx,const char *name,pocket_namespace_fn fn,void *user){
 JSValue root=pocket_api_root(ctx),ns=JS_NewObject(ctx);
 if(JS_IsException(ns)){JS_FreeValue(ctx,root);return ESP_ERR_NO_MEM;}
 esp_err_t status=fn(ctx,ns,user);
 if(status==ESP_OK&&JS_SetPropertyStr(ctx,root,name,JS_DupValue(ctx,ns))<0)status=ESP_ERR_NO_MEM;
 JS_FreeValue(ctx,ns);JS_FreeValue(ctx,root);return status;
}
static ksn_proc_gouraud_patch triangle(uint16_t color){
 ksn_proc_gouraud_patch p={0};p.header.vertices=3;p.header.triangles=1;
 p.vertices[0]=(ksn_proc_gouraud_vertex){10,10,-1,color,color};
 p.vertices[1]=(ksn_proc_gouraud_vertex){30,10,-1,color,color};
 p.vertices[2]=(ksn_proc_gouraud_vertex){10,30,-1,color,color};
 p.indices[0]=0;p.indices[1]=1;p.indices[2]=2;return p;
}
static void append(JSContext *ctx,ksn_proc_gouraud_patch *p,const float *z){
 JSValue v=pocket_proc_native_mesh(ctx,"test",p,z,1);REQUIRE(!JS_IsException(v));JS_FreeValue(ctx,v);
}
static void direct(void){
 uint8_t storage[1024];ksn_mesh_snapshot s={.packets={storage,sizeof storage,0,0},.work_limit=8000000,.enabled=true};
 ksn_proc_gouraud_patch red=triangle(63488),blue=triangle(31);
 float a[3]={0,0,0},b[3]={1,-1,1},one[240],eight[240*8];
 REQUIRE(ksn_mesh_append(&s,&red,a,1)&&ksn_mesh_append(&s,&blue,b,2));
 memset(actual,0,sizeof actual);memset(expected,0,sizeof expected);
 REQUIRE(ksn_mesh_render(&s,actual,0,135,one,1));REQUIRE(ksn_mesh_render(&s,expected,0,135,eight,8));
 REQUIRE(!memcmp(actual,expected,sizeof actual)&&actual[12*240+12]==31&&actual[12*240+25]==63488);
 ksn_mesh_snapshot too_small={.packets={storage,1,0,0},.work_limit=8000000,.enabled=true};
 REQUIRE(!ksn_mesh_append(&too_small,&red,a,1)&&!too_small.packets.size);
 too_small.packets.capacity=sizeof storage;too_small.work_limit=0;REQUIRE(!ksn_mesh_append(&too_small,&red,a,1));
 size_t size=s.packets.size;uint32_t work=s.work;b[0]=NAN;
 REQUIRE(!ksn_mesh_append(&s,&blue,b,2)&&s.packets.size==size&&s.work==work);
 blue.header.holes=1;REQUIRE(!ksn_mesh_append(&s,&blue,a,2));
 s.packets.size=0;s.packets.packets=0;s.work=0;blue.header.holes=0;
 REQUIRE(ksn_mesh_append(&s,&red,a,1)&&ksn_mesh_append(&s,&blue,a,2));
 memset(actual,0,sizeof actual);REQUIRE(ksn_mesh_render(&s,actual,0,135,one,1)&&actual[12*240+12]==63488);
 s.packets.size=0;s.packets.packets=0;s.work=0;
 REQUIRE(ksn_mesh_append(&s,&blue,a,2)&&ksn_mesh_append(&s,&red,a,1));
 memset(actual,0,sizeof actual);REQUIRE(ksn_mesh_render(&s,actual,0,135,one,1)&&actual[12*240+12]==31);
 puts("MESH_DEPTH_RENDER_PASS crossing row1_row8 finite holes immutable_append");
}
int main(void){
 direct();JSRuntime *rt=JS_NewRuntime();REQUIRE(rt);JSContext *ctx=JS_NewContext(rt);REQUIRE(ctx);
 JSValue g=JS_GetGlobalObject(ctx),p=JS_NewObject(ctx);REQUIRE(pocket_proc_install(ctx,p)==ESP_OK);
 REQUIRE(JS_SetPropertyStr(ctx,g,"K",p)>=0);JS_FreeValue(ctx,g);eval_ok(ctx,"globalThis.P=K.procedural;globalThis.pocket={kasane:K}");
 ksn_proc_gouraud_patch red=triangle(63488),blue=triangle(31);float z[3]={0,0,0};
 /* Reciprocal multiplication and per-weight division differ at an authored
  * material boundary even with one face and uniform depth. Compare actual C
  * painter/depth paths, including the complete image in both band sizes. */
 ksn_proc_gouraud_patch boundary={0};boundary.header.vertices=3;boundary.header.triangles=1;
 boundary.vertices[0]=(ksn_proc_gouraud_vertex){0,0,-1,0xf800,0x001f};
 boundary.vertices[1]=(ksn_proc_gouraud_vertex){3,0,2,0xf800,0x001f};
 boundary.vertices[2]=(ksn_proc_gouraud_vertex){0,4,-1,0xf800,0x001f};
 boundary.indices[1]=1;boundary.indices[2]=2;
 eval_ok(ctx,"P.beginFrame(0)");JSValue boundary_result=pocket_proc_native_gouraud(ctx,"boundary",&boundary);
 REQUIRE(!JS_IsException(boundary_result));JS_FreeValue(ctx,boundary_result);eval_ok(ctx,"P.commit()");
 REQUIRE(pocket_proc_backdrop(NULL,0,135,expected)==KSN_OK&&expected[241]==0x001f);pocket_proc_present_result(KSN_OK);
 for(unsigned rows=1;rows<=8;rows+=7){
  eval_ok(ctx,rows==1?"P.beginMeshFrame(0,1024,8000000,1)":"P.beginMeshFrame(0,1024,8000000,8)");
  append(ctx,&boundary,z);eval_ok(ctx,"P.commit()");REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK);
  printf("MESH_SHADER_BOUNDARY rows=%u painter=%04x depth=%04x\n",rows,expected[241],actual[241]);
  REQUIRE(actual[241]==0x001f&&!memcmp(actual,expected,sizeof actual));pocket_proc_present_result(KSN_OK);
 }
 pocket_proc_reset();
 puts("MESH_SHADER_BOUNDARY_PASS single_face uniform_depth legacy_pixel whole_image row1_row8");
 /* One packet must agree with the existing painter coverage, masks and
  * varying A/B vertex shader; depth changes visibility only. */
 ksn_proc_gouraud_patch varying=red;varying.vertices[1].color_a=2016;varying.vertices[2].color_a=31;
 for(unsigned j=0;j<3;j++)varying.vertices[j].color_b=65535;
 varying.vertices[2].mask=1;
 eval_ok(ctx,"P.beginFrame(0)");JSValue legacy=pocket_proc_native_gouraud(ctx,"test",&varying);REQUIRE(!JS_IsException(legacy));JS_FreeValue(ctx,legacy);eval_ok(ctx,"P.commit()");
 REQUIRE(pocket_proc_backdrop(NULL,0,135,expected)==KSN_OK);pocket_proc_present_result(KSN_OK);
 eval_ok(ctx,"P.beginMeshFrame(0,1024,8000000,1)");append(ctx,&varying,z);eval_ok(ctx,"P.commit()");
 REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK&&!memcmp(actual,expected,sizeof actual));pocket_proc_present_result(KSN_OK);pocket_proc_reset();
 pocket_proc_mesh_fail_after(0);eval_error(ctx,"P.beginMeshFrame(0,1024,8000000,1)");
 pocket_proc_mesh_bytes bytes;pocket_proc_mesh_memory(&bytes);REQUIRE(!bytes.candidate&&!bytes.committed&&!bytes.workspace);
 pocket_proc_mesh_fail_after(1);eval_error(ctx,"P.beginMeshFrame(0,1024,8000000,1)");
 pocket_proc_mesh_memory(&bytes);REQUIRE(!bytes.candidate&&!bytes.workspace);pocket_proc_mesh_fail_after(-1);
 eval_ok(ctx,"P.beginMeshFrame(0,1024,8000000,1)");append(ctx,&red,z);
 pocket_proc_mark mark;JSValue v=pocket_proc_native_begin(ctx,"test",&mark);REQUIRE(!JS_IsException(v));JS_FreeValue(ctx,v);
 z[0]=z[1]=z[2]=1;append(ctx,&blue,z);pocket_proc_native_rollback(&mark);
 eval_ok(ctx,"P.commit()");REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK&&actual[12*240+12]==63488);
 pocket_proc_present_result(KSN_OK);
 eval_ok(ctx,"P.beginMeshFrame(0,1024,8000000,8)");append(ctx,&blue,z);eval_ok(ctx,"P.commit()");
 REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK&&actual[12*240+12]==31);
 pocket_proc_present_result(KSN_IO);REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK&&actual[12*240+12]==63488);
 eval_error(ctx,"P.beginFrame(0)");pocket_proc_present_result(KSN_OK);
 pocket_proc_image_mode();eval_ok(ctx,"P.beginMeshFrame(0,1024,8000000,1)");append(ctx,&blue,z);eval_ok(ctx,"P.commit()");
 pocket_proc_present_result(KSN_IO);REQUIRE(pocket_proc_pending());
 ksn_image_port port;pocket_proc_image_port(&port);
 uint16_t pixel;uint8_t alpha;REQUIRE(port.read_span(port.ctx,0,0,12,12,1,&pixel,&alpha)==KSN_OK&&pixel==31&&alpha==255);
 REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK&&actual[12*240+12]==31);
 eval_error(ctx,"P.beginFrame(0)");pocket_proc_present_result(KSN_OK);REQUIRE(!pocket_proc_pending());
 /* Depth-only changes must produce damage even if colour is unchanged. */
 eval_ok(ctx,"P.beginMeshFrame(0,1024,8000000,1)");z[0]=2;append(ctx,&blue,z);eval_ok(ctx,"P.commit()");
 uint32_t damage;int16_t x0[17],x1[17];REQUIRE(pocket_proc_band_damage(0,&damage,x0,x1)&&damage==0x1ffff);
 pocket_proc_present_result(KSN_OK);pocket_proc_mesh_memory(&bytes);
 REQUIRE(bytes.candidate==1024&&bytes.committed==1024&&bytes.workspace==7680);
 printf("MESH_DEPTH_LIFECYCLE_PASS OOM2 rollback backdrop_repair image_retry ACK depth_damage payload=%zu scratch=%zu metadata=%zu\n",bytes.candidate+bytes.committed,bytes.workspace,bytes.metadata);
 pocket_proc_reset();pocket_proc_mesh_memory(&bytes);REQUIRE(!bytes.candidate&&!bytes.committed&&!bytes.workspace);
 /* Both real managed submit paths copy evaluated depth. Destroy their
  * actor/model/input BEFORE rendering the committed candidate. */
 REQUIRE(pocket_shape_install(ctx,NULL)==ESP_OK);
 FILE *f=fopen(".cache/ayatsuri-native/mesh-depth.aynm","rb");REQUIRE(f);REQUIRE(!fseek(f,0,SEEK_END));long len=ftell(f);rewind(f);
 uint8_t *blob=malloc((size_t)len);REQUIRE(blob&&fread(blob,1,(size_t)len,f)==(size_t)len);fclose(f);
 g=JS_GetGlobalObject(ctx);REQUIRE(JS_SetPropertyStr(ctx,g,"source",JS_NewArrayBufferCopy(ctx,blob,(size_t)len))>=0);JS_FreeValue(ctx,g);free(blob);
 eval_ok(ctx,"globalThis.M=pocket.ayatsuri.modelOpen(source);globalThis.A=M.instance();source=null;"
  "A.evaluate(new Float32Array([0,0,400,0,0,0,1,120,78]),new Float32Array([0,0,1,1,1,1]));"
  "P.beginMeshFrame(0,1024,8000000,1);A.submit();P.commit()");
 REQUIRE(pocket_proc_backdrop(NULL,0,135,expected)==KSN_OK);pocket_proc_present_result(KSN_OK);
 eval_ok(ctx,"P.beginMeshFrame(0,1024,8000000,8);A.submitClipped(new Uint16Array(0),new Uint8Array(0),new Float32Array(0));P.commit();A.dispose();M.dispose();A=M=null");JS_RunGC(rt);
 REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK&&!memcmp(actual,expected,sizeof actual));pocket_proc_present_result(KSN_OK);
 pocket_proc_mesh_memory(&bytes);JSMemoryUsage usage;JS_ComputeMemoryUsage(rt,&usage);
 printf("MESH_MANAGED_SUBMIT_PASS submit submitClipped independent_lifetime guest_used=%lld guest_malloc=%lld native_payload=%zu workspace=%zu metadata=%zu\n",(long long)usage.memory_used_size,(long long)usage.malloc_size,bytes.candidate+bytes.committed,bytes.workspace,bytes.metadata);
 pocket_proc_reset();pocket_proc_host_classes_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);return 0;
}
