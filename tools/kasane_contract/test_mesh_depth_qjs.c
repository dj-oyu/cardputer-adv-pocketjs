#include "pocket_shape.h"
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
static size_t sided_actor(JSContext *ctx,const char *name){
 char path[256];snprintf(path,sizeof path,".cache/ayatsuri-native/sided-%s.aynm",name);
 FILE *f=fopen(path,"rb");REQUIRE(f);fseek(f,0,SEEK_END);size_t size=(size_t)ftell(f);rewind(f);
 uint8_t *blob=malloc(size);REQUIRE(blob&&fread(blob,1,size,f)==size);fclose(f);
 JSValue g=JS_GetGlobalObject(ctx);REQUIRE(JS_SetPropertyStr(ctx,g,"sideSource",JS_NewArrayBufferCopy(ctx,blob,size))>=0);JS_FreeValue(ctx,g);free(blob);
 eval_ok(ctx,"globalThis.SM=pocket.ayatsuri.modelOpen(sideSource);globalThis.SA=SM.instance();sideSource=null;"
  "SA.evaluate(new Float32Array([0,0,400,0,0,0,1,0,0]),new Float32Array([0,0,1,1,1,1]));"
  "globalThis.emptyIds=new Uint16Array(0);globalThis.emptyCounts=new Uint8Array(0);globalThis.emptyXY=new Float32Array(0);");
 return size;
}
static pocket_proc_mark side_mark(JSContext *ctx){pocket_proc_mark m;JSValue v=pocket_proc_native_begin(ctx,"sided-test",&m);REQUIRE(!JS_IsException(v));JS_FreeValue(ctx,v);return m;}
static void side_close(JSContext *ctx){eval_ok(ctx,"SA.dispose();SM.dispose();SA=SM=null");pocket_proc_reset();}
static void sided_contracts(JSContext *ctx){
 const char *names[]={"legacy64","sided64","backfirst"};
 for(unsigned n=0;n<3;n++)for(unsigned clipped=0;clipped<2;clipped++){
  size_t cold=sided_actor(ctx,names[n]);const char *build=clipped?"P.beginMeshFrame(17,65536,8000000,1);SA.submitClipped(emptyIds,emptyCounts,emptyXY)":"P.beginMeshFrame(17,65536,8000000,1);SA.submit()";
  pocket_proc_mark mark={0};clock_t start=0;
  for(unsigned sample=0;sample<10;sample++){
   if(sample==2)start=clock();
   eval_ok(ctx,build);mark=side_mark(ctx);eval_ok(ctx,"P.commit()");
   REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK);pocket_proc_present_result(KSN_OK);
  }
  REQUIRE(mark.depth_packets==1u);REQUIRE(actual[12*240+12]==(n==2?0:0xffff));
  printf("SIDED_HOST name=%s submitClipped=%u cold=%zu records=%u snapshot=%zu work=%u warm_submit_render_us=%.0f samples=8\n",names[n],clipped,cold,mark.depth_packets,mark.depth_bytes,mark.depth_work,1e6*(clock()-start)/CLOCKS_PER_SEC/8);
  // Painter observes LAST face; shared depth observes FIRST at exact ties.
  eval_ok(ctx,clipped?"P.beginFrame(17);SA.submitClipped(emptyIds,emptyCounts,emptyXY);P.commit()":"P.beginFrame(17);SA.submit();P.commit()");
  if(n){REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK);REQUIRE(actual[12*240+12]==(n==2?0xffff:0));pocket_proc_present_result(KSN_OK);}
  else pocket_proc_present_result(KSN_OK);
  side_close(ctx);
 }
 // Successful sided copies must survive actor/model disposal before draw.
 for(unsigned clipped=0;clipped<2;clipped++){
  sided_actor(ctx,"backfirst");eval_ok(ctx,clipped?"P.beginMeshFrame(17,1024,8000000,1);SA.submitClipped(emptyIds,emptyCounts,emptyXY);P.commit();SA.dispose();SM.dispose();SA=SM=null":"P.beginMeshFrame(17,1024,8000000,1);SA.submit();P.commit();SA.dispose();SM.dispose();SA=SM=null");
  JS_RunGC(JS_GetRuntime(ctx));REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK&&actual[12*240+12]==0);
  pocket_proc_present_result(KSN_OK);pocket_proc_reset();
 }
 // Five logical meshes now fit as five records, without raising any quota.
 for(unsigned clipped=0;clipped<2;clipped++){
  sided_actor(ctx,"five");eval_ok(ctx,clipped?"P.beginMeshFrame(17,65536,8000000,1);SA.submitClipped(emptyIds,emptyCounts,emptyXY)":"P.beginMeshFrame(17,65536,8000000,1);SA.submit()");
  pocket_proc_mark m=side_mark(ctx);REQUIRE(m.depth_packets==5);eval_ok(ctx,"P.commit()");pocket_proc_present_result(KSN_OK);side_close(ctx);
 }
 // Same unmodified limits:252 previous records + five logical meshes exceeds
 // 256; byte/work limits also fail after partial multi-node append.
 for(unsigned limit=0;limit<3;limit++)for(unsigned clipped=0;clipped<2;clipped++){
  sided_actor(ctx,"five");eval_ok(ctx,limit==1?"P.beginMeshFrame(17,800,8000000,1)":limit==2?"P.beginMeshFrame(17,65536,70000,1)":"P.beginMeshFrame(17,65536,8000000,1)");
  ksn_proc_gouraud_patch prefix=triangle(2016);float z[3]={5,5,5};
  for(unsigned j=0;j<(limit==0?252u:1u);j++)append(ctx,&prefix,z);
  pocket_proc_mark before=side_mark(ctx);
  eval_error(ctx,clipped?"SA.submitClipped(emptyIds,emptyCounts,emptyXY)":"SA.submit()");
  pocket_proc_mark after=side_mark(ctx);
  REQUIRE(after.depth_packets==before.depth_packets&&after.depth_bytes==before.depth_bytes&&after.depth_work==before.depth_work);
  eval_ok(ctx,"P.commit();SA.dispose();SM.dispose();SA=SM=null");
  REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK&&actual[12*240+12]==2016);pocket_proc_present_result(KSN_OK);pocket_proc_reset();
 }
 puts("SIDED_MANAGED_PASS order_tie painter depth both_submit one_mesh_one_record byte_record_work_limits whole_call_rollback independent_lifetime");
}
int main(void){
 direct();JSRuntime *rt=JS_NewRuntime();REQUIRE(rt);JSContext *ctx=JS_NewContext(rt);REQUIRE(ctx);
 JSValue g=JS_GetGlobalObject(ctx),p=JS_NewObject(ctx);REQUIRE(pocket_shiki_api_install(ctx,p)==ESP_OK);
 REQUIRE(JS_SetPropertyStr(ctx,g,"K",p)>=0);JS_FreeValue(ctx,g);eval_ok(ctx,"globalThis.P=K.shiki;globalThis.pocket={kasane:K}");
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
 /* One buffer a surface (docs/vm/qjs-observe.md sec.25): commit() wrote the
  * new frame over the committed one, so a backdrop IO repaints the new one. */
 pocket_proc_present_result(KSN_IO);REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK&&actual[12*240+12]==31);
 eval_error(ctx,"P.beginFrame(0)");pocket_proc_present_result(KSN_OK);
 pocket_proc_image_mode();eval_ok(ctx,"P.beginMeshFrame(0,1024,8000000,1)");append(ctx,&blue,z);eval_ok(ctx,"P.commit()");
 pocket_proc_present_result(KSN_IO);REQUIRE(pocket_proc_pending());
 ksn_image_port port;pocket_proc_image_port(&port);
 /* Direct opaque rows must retain the pending candidate through an IO retry,
  * including 4+3 final rows, exactly like span and backdrop rendering. */
 REQUIRE(port.read_rows);
 REQUIRE(pocket_proc_backdrop(NULL,0,135,expected)==KSN_OK);
 memset(actual,0,sizeof actual);
 for(unsigned y=0;y<135;y+=4){const unsigned rows=135-y<4?135-y:4;
  REQUIRE(port.read_rows(port.ctx,0,0,y,rows,0,240,actual+y*240,240)==KSN_OK);
 }
 REQUIRE(!memcmp(actual,expected,sizeof actual)&&actual[12*240+12]==31);
 REQUIRE(port.read_rows(port.ctx,0,0,0,4,1,239,actual,240)==KSN_UNSUPPORTED);
 REQUIRE(port.read_rows(port.ctx,0,0,134,2,0,240,actual,240)==KSN_INVALID);
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
 /* Shared immutable cold data must not share evaluated instance packets.
  * Deliberately asymmetric cameras make accidental reuse visible. A failed
  * evaluation on A must not invalidate B or the last committed snapshot. */
 static uint16_t independent[240*135];
 eval_ok(ctx,"globalThis.B=M.instance();B.evaluate(new Float32Array([0,0,400,0,0,0,1,103,55]),new Float32Array([0,0,1,1,1,1]));"
  "P.beginMeshFrame(0,1024,8000000,1);B.submit();P.commit()");
 REQUIRE(pocket_proc_backdrop(NULL,0,135,independent)==KSN_OK&&memcmp(independent,expected,sizeof expected));pocket_proc_present_result(KSN_OK);
 eval_ok(ctx,"P.beginMeshFrame(0,1024,8000000,1);A.submit();P.commit()");
 REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK&&!memcmp(actual,expected,sizeof actual));pocket_proc_present_result(KSN_OK);
 eval_error(ctx,"A.evaluate(new Float32Array([0,0,400,0,0,0,0,120,78]),new Float32Array([0,0,1,1,1,1]))");
 eval_error(ctx,"P.beginMeshFrame(0,1024,8000000,1);A.submit()");
 REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK&&!memcmp(actual,expected,sizeof actual));
 eval_ok(ctx,"P.beginMeshFrame(0,1024,8000000,1);B.submitClipped(new Uint16Array(0),new Uint8Array(0),new Float32Array(0));P.commit();B.dispose();B=null");JS_RunGC(rt);
 REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK&&!memcmp(actual,independent,sizeof actual));pocket_proc_present_result(KSN_OK);
 eval_ok(ctx,"A.evaluate(new Float32Array([0,0,400,0,0,0,1,120,78]),new Float32Array([0,0,1,1,1,1]))");
 puts("MESH_SHARED_MODEL_PASS asymmetric_instances evaluation_failure_isolation committed_unchanged recovery disposed_source");
 eval_ok(ctx,"P.beginMeshFrame(0,1024,8000000,8);A.submitClipped(new Uint16Array(0),new Uint8Array(0),new Float32Array(0));P.commit();A.dispose();M.dispose();A=M=null");JS_RunGC(rt);
 REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK&&!memcmp(actual,expected,sizeof actual));pocket_proc_present_result(KSN_OK);
 pocket_proc_mesh_memory(&bytes);JSMemoryUsage usage;JS_ComputeMemoryUsage(rt,&usage);
 printf("MESH_MANAGED_SUBMIT_PASS submit submitClipped independent_lifetime guest_used=%lld guest_malloc=%lld native_payload=%zu workspace=%zu metadata=%zu\n",(long long)usage.memory_used_size,(long long)usage.malloc_size,bytes.candidate+bytes.committed,bytes.workspace,bytes.metadata);
 pocket_proc_reset();sided_contracts(ctx);pocket_proc_host_classes_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);return 0;
}
