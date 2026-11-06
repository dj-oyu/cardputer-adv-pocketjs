/* Real public app, QuickJS and Kasane compositor. Host cycles are protocol stubs. */
#include "pocket_kasane.h"
#include "pocket_shiki_frame.h"
#include "pocket_shape.h"
#include "pocket_shiki_api.h"
#include "app_registry.h"
#include "shiki_heap.h"
#include "ksn_render.h"
#include "ksn_mesh_depth.h"
#include "ksn_font.h"
#ifdef KSN_MESH_Q7_CERTIFIED_PROTOTYPE
#include "ksn_mesh_q7_certified.h"
extern const ksn_mesh_q7_snapshot *pocket_proc_q7_host_snapshot(unsigned,bool);
extern void pocket_proc_q7_bench_stats(ksn_mesh_q7_stats *);
#endif
#include "pocket_av.h"
#include "system/sys_device.h"
#include "sound.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_requested_alloc.h"
#include "meshbench_eval_alloc.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%s:%d %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)
void host_capabilities_clear(void);
int32_t pocket_av_ui_current_player(void){return 0;}
bool pocket_av_ui_read(int32_t id,pocket_av_ui_snapshot *o){(void)id;(void)o;return false;}
bool sys_device_clock_read(sys_clock_state *o){(void)o;return false;}
void sound_stream_set_observer(sound_stream_observer_fn o){(void)o;}
void sound_stream_set_observer_interval(sound_stream_observer_fn o,uint32_t n){(void)o;(void)n;}
static uint16_t strip[240*8],panel[240*135],reference[240*135];
static unsigned strip_rows,half,ready,start,ends,samples,presents;
static bool fail_send;
static bool delayed,dma_pending,dma_candidate;
static const uint16_t *dma_loan;
static uint16_t dma_saved[240*8],dma_y,dma_rows;
static unsigned dma_finishes;
static uint32_t clock_value=0xffffff00u;
static void dma_flush(void){
 if(!dma_pending)return;
 CHECK(!memcmp(dma_loan,dma_saved,(size_t)dma_rows*240*2));
 memcpy(panel+dma_y*240,dma_loan,(size_t)dma_rows*240*2);dma_pending=false;
}
static uint16_t *get_strip(void *u){(void)u;uint16_t *p=strip+(strip_rows==4?(half++&1u)*960u:0);CHECK(!delayed||strip_rows!=4||!dma_pending||p!=dma_loan);for(unsigned i=0;i<strip_rows*240;i++)p[i]=0xdead;return p;}
static ksn_result send_strip(void *u,uint16_t y,uint16_t rows,const uint16_t *p){
 (void)u;
 if(delayed){dma_flush();memcpy(dma_saved,p,(size_t)rows*240*2);dma_loan=strip_rows==4?p:dma_saved;dma_y=y;dma_rows=rows;dma_pending=true;}
 else memcpy(panel+y*240,p,(size_t)rows*240*2);
 if(fail_send){fail_send=false;return KSN_IO;}return KSN_OK;
}
static ksn_result dma_finish(void *u){(void)u;if(dma_candidate)CHECK(pocket_proc_pending());dma_flush();dma_finishes++;return KSN_OK;}
static ksn_result present(void){ksn_display_port p={.strip=get_strip,.present=send_strip,.width=240,.height=135,.strip_rows=strip_rows,.text=&ksn_font_port};ksn_render_stats s;presents++;dma_candidate=pocket_proc_pending();const ksn_display_completion completion={NULL,dma_finish};ksn_result result=delayed?pocket_kasane_present_complete(&p,&completion,&s):pocket_kasane_present(&p,&s);CHECK(!dma_pending);dma_candidate=false;return result;}
static JSValue log_line(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
 (void)self;const char *s=argc?JS_ToCString(c,argv[0]):NULL;CHECK(s);
 if(!strncmp(s,"MB READY ",9))ready++;
 else if(!strcmp(s,"MB SAMPLE_START f=30"))start++;
 else if(!strncmp(s,"MB SAMPLE ",10))samples++;
 else if(!strcmp(s,"MB END n=120 evaluate_cy=12000 submit_commit_cy=12000"))ends++;
 else CHECK(0);
 puts(s);JS_FreeCString(c,s);return JS_UNDEFINED;
}
static JSValue cycles(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){(void)self;(void)argc;(void)argv;clock_value+=100;return JS_NewUint32(c,clock_value);}
static JSValue build(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){(void)self;(void)argc;(void)argv;return JS_NewString(c,"HOST_COUNTER_STUB");}
static void eval(JSContext *c,const char *s){
 JSValue v=JS_Eval(c,s,strlen(s),"meshbench-host",JS_EVAL_TYPE_GLOBAL);
 if(JS_IsException(v)){JSValue e=JS_GetException(c);const char *m=JS_ToCString(c,e);fprintf(stderr,"%s\n",m?m:"JS error");exit(1);}
 JS_FreeValue(c,v);pocket_kasane_end_turn();
}
static void expected_frame(unsigned f){
 static uint8_t bytes[4096];static float scratch[240];
 ksn_mesh_snapshot s={.packets={bytes,sizeof bytes,0,0},.work_limit=200000,.enabled=true};
 const uint16_t colors[4]={0xf800,0x07e0,0x001f,0xffe0};
 for(unsigned k=0;k<4;k++){
  const float z=k%2?4:-4,x=-24+16*(int)k+112+(f%16),y=-8+8*(int)k+64+((f>>4)%4);
  ksn_proc_gouraud_patch p={0};p.header.vertices=4;p.header.triangles=2;
  const float xy[8]={x-40,y-28,x+40,y-28,x+40,y+28,x-40,y+28};float depth[4]={z+(int)k-402,-z+(int)k-402,-z+(int)k-402,z+(int)k-402};
  for(unsigned v=0;v<4;v++)p.vertices[v]=(ksn_proc_gouraud_vertex){xy[2*v],xy[2*v+1],-1,colors[k],colors[k]};
  const uint16_t indices[6]={0,1,2,0,2,3};memcpy(p.indices,indices,sizeof indices);CHECK(ksn_mesh_append(&s,&p,depth,k+1));
 }
 for(unsigned i=0;i<240*135;i++)reference[i]=0x0841;
 CHECK(ksn_mesh_render(&s,reference,0,135,scratch,1));
}
int main(int argc,char **argv){
 CHECK(argc==3||argc==4);strip_rows=(unsigned)atoi(argv[1]);g_ksn_image_rows=atoi(argv[2]);delayed=argc==4&&atoi(argv[3]);CHECK(strip_rows==4||strip_rows==8);
 const app_manifest_t *manifest=app_registry_find("local.meshbench");CHECK(manifest&&app_registry_wants(manifest,"kasane.shapes")&&app_registry_wants(manifest,"kasane.shiki.debug"));
 static char source[8192];const char *source_path=getenv("MESHBENCH_SOURCE");FILE *f=fopen(source_path?source_path:"apps/meshbench/mesh_bench.js","rb");CHECK(f);size_t n=fread(source,1,sizeof source-1,f);CHECK(feof(f));fclose(f);source[n]=0;
 JSRuntime *rt=JS_NewRuntime2(&MB_ALLOC,NULL);CHECK(rt);JSContext *ctx=JS_NewContext(rt);CHECK(ctx&&JS_PocketHarden(ctx)==0);JS_SetMemoryLimit(rt,160*1024);
 host_capabilities_clear();CHECK(shiki_heap_begin(0));CHECK(pocket_kasane_install(ctx,NULL)==ESP_OK);CHECK(pocket_shape_install(ctx,NULL)==ESP_OK);
 JSValue g=JS_GetGlobalObject(ctx),d=JS_NewObject(ctx);JS_SetPropertyStr(ctx,d,"cycles",JS_NewCFunction(ctx,cycles,"cycles",0));JS_SetPropertyStr(ctx,d,"benchBuild",JS_NewCFunction(ctx,build,"benchBuild",0));JS_SetPropertyStr(ctx,g,"shikiDebug",d);JS_SetPropertyStr(ctx,g,"__log",JS_NewCFunction(ctx,log_line,"log",1));JS_FreeValue(ctx,g);
 eval(ctx,"globalThis.pocket={kasane,ayatsuri,shikiDebug};globalThis.console={log:__log}");
 const char *ceiling=getenv("MESHBENCH_REQUEST_CEILING");mb_ceiling=ceiling?(size_t)strtoul(ceiling,NULL,10):0;
 size_t eval_before=mb_live;mb_peak=mb_live;mb_tracking=true;
 eval(ctx,source);mb_tracking=false;CHECK(ready==1&&!mb_rejected);
 printf("MESH_BENCH_EVAL usable_before=%zu usable_peak=%zu usable_after=%zu largest_request=%zu ceiling=%zu rejected=%zu host_pointer_bytes=%zu host_jsvalue_bytes=%zu tlsf_length_model_only=1\n",eval_before,mb_peak,mb_live,mb_max_request,mb_ceiling,mb_rejected,sizeof(void*),sizeof(JSValue));CHECK(present()==KSN_OK);
 uint32_t digest=2166136261u;size_t guest_peak=0;
 for(unsigned frame=0;frame<150;frame++){
  eval(ctx,"frame()");CHECK(pocket_proc_pending());
  if(frame==79){fail_send=true;CHECK(present()==KSN_IO&&pocket_proc_pending());}
  CHECK(present()==KSN_OK&&!pocket_proc_pending());expected_frame(frame);CHECK(!memcmp(panel,reference,sizeof panel));
#ifdef KSN_MESH_Q7_CERTIFIED_PROTOTYPE
  const ksn_mesh_q7_snapshot *q=pocket_proc_q7_host_snapshot(0,true);
  CHECK(q&&q->valid&&!q->fallback&&q->count==8);
  ksn_mesh_q7_stats st;pocket_proc_q7_bench_stats(&st);
  CHECK(st.depth_fast&&st.mask_fast&&!st.frame_fallback);
#endif
  for(unsigned i=0;i<240*135;i++){digest^=panel[i];digest*=16777619u;}
  size_t used,limit;JS_GetMemoryCounters(rt,&used,&limit);if(used>guest_peak)guest_peak=used;CHECK(limit==160*1024);
 }
 CHECK(start==1&&ends==0&&samples==4);unsigned before=presents;eval(ctx,"frame();frame()");CHECK(ends==1&&!pocket_proc_pending()&&!pocket_kasane_needs_present()&&presents==before);
 pocket_proc_mesh_bytes memory;pocket_proc_mesh_memory(&memory);
 printf("MESH_BENCH_HOST strip=%u image_rows=%d frames=150 digest=%08x requested_live=%zu requested_peak=%zu guest_frame_sampled_peak=%zu capacity=%zu workspace=%zu metadata=%zu\n",strip_rows,g_ksn_image_rows,digest,live_bytes,peak_bytes,guest_peak,memory.candidate+memory.committed,memory.workspace,memory.metadata);
 const char *capture=getenv("MESHBENCH_CAPTURE_RGB565");
 if(capture){FILE *out=fopen(capture,"wb");CHECK(out);for(unsigned i=0;i<240*135;i++){CHECK(fputc(panel[i]&255,out)!=EOF);CHECK(fputc(panel[i]>>8,out)!=EOF);}CHECK(fclose(out)==0);}
 pocket_shiki_api_reset();pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);shiki_heap_end();host_capabilities_clear();CHECK(!live_bytes&&!live_blocks&&!mb_live);
 if(delayed){CHECK(dma_finishes>=151);printf("MESH_BENCH_DELAYED_PASS strip=%u borrowed_half=%u completion_before_ACK finishes=%u\n",strip_rows,strip_rows==4,dma_finishes);}
 puts("MESH_BENCH_HOST_PASS drawing_reference row_span IO_ACK bounded_END release0 host_cycles_are_stubs");return 0;
}
