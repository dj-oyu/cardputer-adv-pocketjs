// apps/keytest/keytest.js against the real QuickJS, the real pocket.kasane and
// the real pocket.input.keys, with key edges fed through the real keymap_poll()
// and keystate. What this catches before a flash: a scene Kasane refuses, an
// exception in a turn, and the app's own bookkeeping (KEYTEST_SKEW) disagreeing
// with what input.keys reported for a scripted run of chords, taps and an
// overflow. It is the host half of the key test; the device half -- what the
// matrix really reports -- is docs/platform/keystate.md's list.
//
//   wsl -e bash -lc "cd <repo> && bash tools/build_keytest_app_test.sh && /tmp/test-keytest-app"
#include "pocket_kasane.h"
#include "pocket_input.h"
#include "pocket_api.h"
#include "pocket_av.h"
#include "keymap.h"
#include "system/sys_device.h"
#include "ui/kasane/ksn_render.h"
#include "text/ksn_font.h"
#include "sound.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
/* Generated verbatim from pocket_api.c: production subscription semantics. */
#include "pocket_sub_impl.inc"

void host_capabilities_clear(void);
int32_t pocket_av_ui_current_player(void){return 0;}
bool pocket_av_ui_read(int32_t id,pocket_av_ui_snapshot *out){(void)id;(void)out;return false;}
bool sys_device_clock_read(sys_clock_state *out){(void)out;return false;}
void sound_stream_set_observer(sound_stream_observer_fn o){(void)o;}
void sound_stream_set_observer_interval(sound_stream_observer_fn o,uint32_t n){(void)o;(void)n;}
void *__real_calloc(size_t count,size_t size);
void __real_free(void *ptr);
void __wrap_free(void *ptr){__real_free(ptr);}
void *__wrap_calloc(size_t count,size_t size){return __real_calloc(count,size);}
bool pocket_text_active(void){return false;}
int64_t esp_timer_get_time(void){return 0;}
// pocket.kasane's image surfaces, which this app does not use. Stubbed rather
// than linked: they pull the SD video stream and the grid assets with them.
esp_err_t pocket_grid_install(JSContext *c,JSValueConst k){(void)c;(void)k;return ESP_OK;}
void pocket_grid_reset(void){}
bool pocket_grid_pending(void){return false;}
void pocket_grid_present_result(ksn_result r){(void)r;}
void pocket_grid_source_invalidated(uint32_t id){(void)id;}
esp_err_t pocket_video_install(JSContext *c,JSValueConst k){(void)c;(void)k;return ESP_OK;}
void pocket_video_reset(void){}
bool pocket_video_pending(void){return false;}
void pocket_video_present_result(ksn_result r){(void)r;}
esp_err_t pocket_pixel_install(JSContext *c,JSValueConst k){(void)c;(void)k;return ESP_OK;}
void pocket_pixel_reset(void){}
bool pocket_pixel_pending(void){return false;}
void pocket_pixel_present_result(ksn_result r){(void)r;}

static board_keyevent_t fifo[8];
static unsigned fifo_n,fifo_at;
bool board_key_event(board_keyevent_t *out){
    if(fifo_at==fifo_n){fifo_n=fifo_at=0;return false;}
    *out=fifo[fifo_at++];return true;
}
static void edge(const char *name,bool pressed,bool overflow){
    int i=keymap_key_index(name);
    if(fifo_at==fifo_n)fifo_n=fifo_at=0;
    fifo[fifo_n++]=(board_keyevent_t){.row=(uint8_t)(i/14),.col=(uint8_t)(i%14),
                                      .pressed=pressed,.overflow=overflow};
    keystroke_t k;keymap_poll(&k);
}

static JSRuntime *rt;
static JSContext *ctx;
static uint16_t strip_pixels[240*8];
static unsigned exceptions,ready,lines,skew,last_max,last_p,last_r;
static JSValue js_log(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *s=argc?JS_ToCString(c,argv[0]):NULL;
    if(!s)return JS_UNDEFINED;
    printf("  %s\n",s);
    if(!strncmp(s,"KEYTEST_READY cap=true/true keys=56",35))ready++;
    else if(!strncmp(s,"KEYTEST_SKEW",12))skew++;
    else if(!strncmp(s,"KEYTEST f=",10)){
        lines++;
        const char *m=strstr(s," max=");if(m)sscanf(m," max=%u p=%u r=%u",&last_max,&last_p,&last_r);
    }
    JS_FreeCString(c,s);
    return JS_UNDEFINED;
}
static bool eval(const char *src,size_t len,const char *name){
    JSValue v=JS_Eval(ctx,src,len,name,JS_EVAL_TYPE_GLOBAL);
    bool ok=!JS_IsException(v);
    if(!ok){
        JSValue e=JS_GetException(ctx);const char *t=JS_ToCString(ctx,e);
        printf("  %s threw: %s\n",name,t?t:"?");if(t)JS_FreeCString(ctx,t);
        JS_FreeValue(ctx,e);exceptions++;
    }
    JS_FreeValue(ctx,v);
    JSContext *c;while(JS_ExecutePendingJob(rt,&c)>0){}
    pocket_kasane_end_turn();
    return ok;
}
static uint16_t *get_strip(void *o){(void)o;return strip_pixels;}
static ksn_result send_strip(void *o,uint16_t y,uint16_t rows,const uint16_t *p){
    (void)o;(void)y;(void)rows;(void)p;return KSN_OK;
}
static unsigned bad_present;
static void present(void){
    if(!pocket_kasane_needs_present())return;
    ksn_display_port port={.strip=get_strip,.present=send_strip,.width=240,.height=135,
                           .strip_rows=8,.text=&ksn_font_port};
    ksn_render_stats stats;
    if(pocket_kasane_present(&port,&stats)!=KSN_OK)bad_present++;
}
static void frame(void){
    pocket_input_pump(0);
    eval("frame(0)",8,"frame.js");
    present();
}

static const char STUB[]=
    "globalThis.console={log:globalThis.__log};"
    "globalThis.pocket={kasane:globalThis.kasane,input:globalThis.input,"
    "capabilities:{get:n=>({name:n,supported:__cap(n),available:__cap(n)})}};";
static JSValue js_cap(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *n=argc?JS_ToCString(c,argv[0]):NULL;
    bool yes=n&&pocket_api_supported(n);
    if(n)JS_FreeCString(c,n);
    return JS_NewBool(c,yes);
}

int main(void){
    FILE *f=fopen("apps/keytest/keytest.js","rb");
    if(!f){printf("cannot open apps/keytest/keytest.js\n");return 2;}
    static char src[1<<14];size_t n=fread(src,1,sizeof src-1,f);fclose(f);src[n]=0;
    rt=JS_NewRuntime();ctx=JS_NewContext(rt);host_capabilities_clear();
    pocket_kasane_install(ctx,NULL);
    pocket_input_install(ctx,NULL);
    JSValue g=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,g,"__log",JS_NewCFunction(ctx,js_log,"log",1));
    JS_SetPropertyStr(ctx,g,"__cap",JS_NewCFunction(ctx,js_cap,"cap",1));
    JS_FreeValue(ctx,g);
    eval(STUB,strlen(STUB),"stub.js");
    edge("enter",true,false);            // the key that launched it
    bool ok=eval(src,n,"keytest.js");present();
    frame();
    edge("enter",false,false);frame();   // hidden: no line
    edge("e",true,false);edge("a",true,false);edge("s",true,false);edge("d",true,false);frame();
    edge("space",true,false);frame();
    edge("space",false,false);frame();
    edge("k",true,false);edge("k",false,false);frame();          // a tap within a frame
    edge("shift",true,false);edge("l",true,false);frame();
    edge("f",true,true);frame();                                 // overflow releases the rest
    edge("e",false,false);edge("a",false,false);frame();         // late releases: duplicates
    edge("f",false,false);frame();
    for(int t=0;t<30;t++)frame();
    printf("keytest host: ready=%u lines=%u skew=%u max=%u p=%u r=%u exceptions=%u bad_present=%u\n",
           ready,lines,skew,last_max,last_p,last_r,exceptions,bad_present);
    // Presses: e a s d space k shift l f = 9. Releases: space k, then the
    // overflow's e a s d shift l, then f = 9. Most at once: e a s d shift l = 6.
    // Seven lines: the late releases (frame 9) and the idle frames print none.
    bool pass=ok&&ready==1&&!skew&&!exceptions&&!bad_present&&lines==7&&
              last_max==6&&last_p==9&&last_r==9;
    pocket_input_reset();pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
    printf("%s\n",pass?"KEYTEST_HOST PASS":"KEYTEST_HOST FAIL");
    return pass?0:1;
}
