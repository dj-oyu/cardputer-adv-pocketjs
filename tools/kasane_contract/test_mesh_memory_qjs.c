/* Public synthetic host probe. Requested bytes exclude allocator overhead. */
#define main unused_frame_contract_main
#include "test_pocket_proc_qjs.c"
#undef main
#include "ksn_mesh_depth.h"
#include <stdint.h>
#include "host_requested_alloc.h"
static ksn_proc_gouraud_patch triangle(uint16_t c){
 ksn_proc_gouraud_patch p={0};p.header.vertices=3;p.header.triangles=1;
 p.vertices[0]=(ksn_proc_gouraud_vertex){10,10,-1,c,c};
 p.vertices[1]=(ksn_proc_gouraud_vertex){30,10,-1,c,c};
 p.vertices[2]=(ksn_proc_gouraud_vertex){10,30,-1,c,c};p.indices[1]=1;p.indices[2]=2;return p;
}
static void append(JSContext *ctx,uint16_t color,float depth){
 ksn_proc_gouraud_patch p=triangle(color);float z[3]={depth,depth,depth};
 JSValue v=pocket_proc_native_mesh(ctx,"memory-probe",&p,z,1);REQUIRE(!JS_IsException(v));JS_FreeValue(ctx,v);
}
static void compare_backdrop(uint16_t foreground,uint16_t background){
 REQUIRE(pocket_proc_backdrop(NULL,0,135,expected)==KSN_OK);
 for(unsigned step=4;step<=8;step+=4){
  memset(actual,0,sizeof actual);
  for(unsigned y=0;y<135;y+=step){unsigned n=135-y<step?135-y:step;
   REQUIRE(pocket_proc_backdrop(NULL,y,n,actual+y*240)==KSN_OK);
  }REQUIRE(!memcmp(actual,expected,sizeof actual));
 }
 REQUIRE(actual[12*240+12]==foreground&&actual[0]==background);
}
static void compare_ports(ksn_image_port *port,uint16_t foreground,uint16_t background){
 REQUIRE(pocket_proc_backdrop(NULL,0,135,expected)==KSN_OK);
 for(unsigned step=4;step<=8;step+=4){
  memset(actual,0,sizeof actual);
  for(unsigned y=0;y<135;y+=step){unsigned n=135-y<step?135-y:step;
   REQUIRE(port->read_rows(port->ctx,0,0,y,n,0,240,actual+y*240,240)==KSN_OK);
  }REQUIRE(!memcmp(actual,expected,sizeof actual));
  memset(actual,0,sizeof actual);
  for(unsigned y=0;y<135;y+=step){unsigned n=135-y<step?135-y:step;
   REQUIRE(pocket_proc_backdrop(NULL,y,n,actual+y*240)==KSN_OK);
  }REQUIRE(!memcmp(actual,expected,sizeof actual));
 }
 for(unsigned y=0;y<135;y++)for(unsigned x=0;x<240;x+=32){unsigned n=240-x<32?240-x:32;uint8_t alpha[32];
  REQUIRE(port->read_span(port->ctx,0,0,y,x,n,actual+y*240+x,alpha)==KSN_OK);
  for(unsigned k=0;k<n;k++)REQUIRE(alpha[k]==255);
 }REQUIRE(!memcmp(actual,expected,sizeof actual));
 REQUIRE(actual[12*240+12]==foreground&&actual[0]==background);
}
void pocket_proc_host_classes_reset(void);
int main(void){
 for(unsigned rows=1;rows<=8;rows+=7){
  REQUIRE(live_bytes==0&&live_blocks==0);peak_bytes=alloc_calls=0;
  JSRuntime *rt=JS_NewRuntime();REQUIRE(rt);JSContext *ctx=JS_NewContext(rt);REQUIRE(ctx);
  JSValue g=JS_GetGlobalObject(ctx),p=JS_NewObject(ctx);REQUIRE(pocket_shiki_api_install(ctx,p)==ESP_OK);
  REQUIRE(JS_SetPropertyStr(ctx,g,"K",p)>=0);JS_FreeValue(ctx,g);eval_ok(ctx,"globalThis.P=K.shiki");
  pocket_proc_image_mode();char begin[100];const uint16_t colors[3]={31,2016,65535};
  for(unsigned cycle=0;cycle<3;cycle++){
   const uint16_t bg=(uint16_t)(17+cycle),fg=colors[cycle];
   snprintf(begin,sizeof begin,"P.beginMeshFrame(%u,1024,8000000,%u)",bg,rows);
   eval_ok(ctx,begin);append(ctx,63488,0);append(ctx,fg,1);eval_ok(ctx,"P.commit()");
   ksn_image_port port;pocket_proc_image_port(&port);compare_ports(&port,fg,bg);
   pocket_proc_present_result(KSN_IO);REQUIRE(pocket_proc_pending());compare_ports(&port,fg,bg);
   eval_error(ctx,"P.beginFrame(0)");pocket_proc_present_result(KSN_OK);REQUIRE(!pocket_proc_pending());compare_ports(&port,fg,bg);
  }
  /* Backdrop-mode IO discards the candidate and repairs old committed data,
   * unlike image-mode retry. Distinct colors detect stale snapshot selection. */
  pocket_proc_reset();
  snprintf(begin,sizeof begin,"P.beginMeshFrame(24,1024,8000000,%u)",rows);
  eval_ok(ctx,begin);append(ctx,31,1);eval_ok(ctx,"P.commit()");
  compare_backdrop(31,24);pocket_proc_present_result(KSN_OK);
  snprintf(begin,sizeof begin,"P.beginMeshFrame(25,1024,8000000,%u)",rows);
  eval_ok(ctx,begin);append(ctx,2016,1);eval_ok(ctx,"P.commit()");compare_backdrop(2016,25);
  pocket_proc_present_result(KSN_IO);REQUIRE(!pocket_proc_pending());compare_backdrop(31,24);
  eval_error(ctx,"P.beginFrame(0)");pocket_proc_present_result(KSN_OK);compare_backdrop(31,24);
  pocket_proc_mesh_bytes bytes;pocket_proc_mesh_memory(&bytes);JSMemoryUsage usage;JS_ComputeMemoryUsage(rt,&usage);
  printf("HOST_MEMORY rows=%u pointer_bits=%zu tracked_requested_live=%zu peak=%zu blocks=%zu successful_alloc_calls=%zu guest_used=%lld guest_malloc=%lld snapshot_capacity=%zu workspace=%zu metadata=%zu\n",rows,8*sizeof(void *),live_bytes,peak_bytes,live_blocks,alloc_calls,(long long)usage.memory_used_size,(long long)usage.malloc_size,bytes.candidate+bytes.committed,bytes.workspace,bytes.metadata);
  pocket_proc_reset();pocket_proc_mesh_memory(&bytes);REQUIRE(!bytes.candidate&&!bytes.committed&&!bytes.workspace);
  pocket_proc_host_classes_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
  printf("HOST_MEMORY_RELEASE rows=%u tracked_live=%zu blocks=%zu peak=%zu\n",rows,live_bytes,live_blocks,peak_bytes);REQUIRE(!live_bytes&&!live_blocks);
 }
 puts("HOST_MEMORY_ROWS_SPAN_BACKDROP_PASS synthetic_3cycles changed_colors image_retry ACK backdrop_repair row4_row8 final3 final7 reset");return 0;
}
