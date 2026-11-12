/* ESP32-S3 QEMU diagnostic: actual C chunk loader, QuickJS, Scene/native/render.
 * API namespace/admission and unavailable peripherals use existing test stubs. */
#include "pocket_kasane.h"
#include "pocket_shape.h"
#include "pocket_shiki_api.h"
#include "pocket_shiki_frame.h"
#include "pocket_app_load.h"
#include "app_registry.h"
#include "shiki_heap.h"
#include "ksn_render.h"
#include "ksn_font.h"
#include "pocket_av.h"
#include "system/sys_device.h"
#include "sound.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void host_capabilities_clear(void);
int32_t pocket_av_ui_current_player(void){return 0;}
bool pocket_av_ui_read(int32_t id,pocket_av_ui_snapshot *out){(void)id;(void)out;return false;}
bool sys_device_clock_read(sys_clock_state *out){(void)out;return false;}
void sound_stream_set_observer(sound_stream_observer_fn o){(void)o;}
void sound_stream_set_observer_interval(sound_stream_observer_fn o,uint32_t n){(void)o;(void)n;}
static const app_manifest_t diagnostic={.id="local.skinasset",.title="skin asset diagnostic",.entry="skin_asset_bootstrap.js",.runtime=APP_RUNTIME_POCKET};
const app_manifest_t *app_registry_current(void){return &diagnostic;}
extern const char bootstrap_start[] asm("_binary_skin_asset_bootstrap_js_start");
extern const char bootstrap_end[] asm("_binary_skin_asset_bootstrap_js_end");
static uint16_t strip[240*4];static uint32_t hash;static unsigned coverage,pixels;
static uint16_t *get_strip(void *u){(void)u;return strip;}
static ksn_result send_strip(void *u,uint16_t y,uint16_t rows,const uint16_t *p){
 (void)u;(void)y;for(unsigned n=0;n<(unsigned)rows*240;n++){hash=(hash^p[n])*16777619u;coverage+=p[n]!=17;pixels++;}return KSN_OK;
}
static bool present(void){
 hash=2166136261u;coverage=pixels=0;
 ksn_display_port port={.strip=get_strip,.present=send_strip,.width=240,.height=135,.strip_rows=4,.text=&ksn_font_port};ksn_render_stats stats;
 return pocket_kasane_present(&port,&stats)==KSN_OK;
}
static bool value_ok(JSContext *ctx,JSValue v){bool ok=!JS_IsException(v);if(!ok){JSValue e=JS_GetException(ctx);const char *s=JS_ToCString(ctx,e);printf("SKINASSET_JS_FAIL %s\n",s?s:"exception");JS_FreeCString(ctx,s);JS_FreeValue(ctx,e);}JS_FreeValue(ctx,v);pocket_kasane_end_turn();return ok;}
static bool eval(JSContext *ctx,const char *s,size_t n){return value_ok(ctx,JS_Eval(ctx,s,n,"skinasset-sim",JS_EVAL_TYPE_GLOBAL));}
static bool call(JSContext *ctx,JSValue fn,int ms){JSValue arg=JS_NewInt32(ctx,ms);return value_ok(ctx,JS_Call(ctx,fn,JS_UNDEFINED,1,&arg));}
void app_main(void){
 bool ok=true;unsigned frames=0;printf("SKINASSET_BEGIN contract=31013b0e mesh=0 vertices=6 joints=2 scalar_qemu c_chunk_loader namespace_stub\n");
 JSRuntime *rt=JS_NewRuntime();if(!rt){puts("SKINASSET_FAIL runtime");return;}JSContext *ctx=JS_NewContext(rt);if(!ctx){JS_FreeRuntime(rt);puts("SKINASSET_FAIL context");return;}
 JS_SetMemoryLimit(rt,160*1024);JS_SetMaxStackSize(rt,32*1024);
 host_capabilities_clear();ok=JS_PocketHarden(ctx)==0&&shiki_heap_begin(0);
 ok=ok&&pocket_kasane_install(ctx,NULL)==ESP_OK&&pocket_shape_install(ctx,NULL)==ESP_OK&&pocket_app_load_install(ctx)==ESP_OK;
 const char *setup="globalThis.pocket={kasane,ayatsuri,app};";if(ok)ok=eval(ctx,setup,strlen(setup));
 if(ok)ok=eval(ctx,bootstrap_start,(size_t)(bootstrap_end-bootstrap_start-1));
 JSValue global=JS_GetGlobalObject(ctx),frame=JS_GetPropertyStr(ctx,global,"skinAssetFrame");
 uint32_t hashes[2]={0};for(unsigned f=0;ok&&f<120;f++){
  unsigned pose=(f/30)%2;ok=call(ctx,frame,pose?1000:0)&&present();
  if(ok)ok=pixels==240*135&&coverage==(pose?1680:1590);
  if(ok){if(!hashes[pose])hashes[pose]=hash;else ok=hash==hashes[pose];}
  if(f==0||f==30||f==60||f==90)printf("SKINASSET_FRAME f=%u pose=%u coverage=%u pixels=%u hash=%08lx ok=%d\n",f,pose,coverage,pixels,(unsigned long)hash,ok);
  frames++;vTaskDelay(1);
 }
 if(ok)ok=hashes[0]!=hashes[1];
 const char *stats="const st={};skinAssetStats(st);if(st.skinWorkBytes!==480)throw Error('skinWork');";if(ok)ok=eval(ctx,stats,strlen(stats));
 if(ok) { ok=call(ctx,frame,1000); }
 const char *stop="skinAssetStop();skinAssetStop();";
 if(ok) { ok=eval(ctx,stop,strlen(stop)); }
 JS_RunGC(rt);
 if(ok)ok=pocket_proc_pending()&&present()&&coverage==1680&&hash==hashes[1];
 size_t used,limit;JS_GetMemoryCounters(rt,&used,&limit);JSOOMCanary canary={0};JS_TakeOOMCanary(rt,&canary);if(canary.count)ok=false;
 printf("SKINASSET_RESULT ok=%d frames=%u guest=%u limit=%u oom=%lu skinWork=480 pending_stop=%d\n",ok,frames,(unsigned)used,(unsigned)limit,(unsigned long)canary.count,!pocket_proc_pending());
 JS_FreeValue(ctx,frame);JS_FreeValue(ctx,global);pocket_shiki_api_reset();pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);shiki_heap_end();host_capabilities_clear();
 puts(ok?"SKINASSET_DONE PASS":"SKINASSET_DONE FAIL");
}
