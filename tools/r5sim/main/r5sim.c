// STRESS's turns on the chip, for QEMU (docs/vm/r5-icache.md). The setup is
// tools/test_stress_app.c's -- real QuickJS, real pocket.kasane, a pocket.fs
// stub -- but frame() is called as app_tick calls it (JS_Call, then the job
// queue drained, then pocket_kasane_end_turn), not through JS_Eval, so the
// parser does not run inside a turn the way it does on the host.
//
// The turns are marked for the trace by calling r5_turn_begin/r5_turn_end:
// QEMU's exec log names a translation block by its address, and these two
// are the only code at theirs, so tools/r5sim/icache.py cuts the fetch stream
// at them without any clock or output having to line up.
#include "pocket_kasane.h"
#include "pocket_memory.h"
#include "pocket_av.h"
#include "system/sys_device.h"
#include "ui/kasane/ksn_runtime.h"
#include "ui/kasane/ksn_render.h"
#include "text/ksn_font.h"
#include "sound.h"
#include <stdio.h>
#include <string.h>

void host_capabilities_clear(void);
int32_t pocket_av_ui_current_player(void){return 0;}
bool pocket_av_ui_read(int32_t id,pocket_av_ui_snapshot *out){(void)id;(void)out;return false;}
bool sys_device_clock_read(sys_clock_state *out){(void)out;return false;}
void sound_stream_set_observer(sound_stream_observer_fn o){(void)o;}
void sound_stream_set_observer_interval(sound_stream_observer_fn o,uint32_t n){(void)o;(void)n;}

extern const char stress_js_start[] asm("_binary_stress_js_start");
extern const char stress_js_end[] asm("_binary_stress_js_end");

#ifndef R5SIM_FRAMES
#define R5SIM_FRAMES 120
#endif

// Distinct bodies so the linker cannot fold them into one address.
volatile unsigned r5_turn_now, r5_turn_done;
__attribute__((noinline,used)) void r5_turn_begin(unsigned t){ r5_turn_now=t; }
__attribute__((noinline,used)) void r5_turn_end(unsigned t){ r5_turn_done=t+1; }

static JSRuntime *rt;
static JSContext *ctx;
static uint16_t strip_pixels[240*8];
static unsigned oom_lines, fail_lines;

static JSValue js_log(JSContext *c,JSValueConst self,int argc,JSValueConst *argv) {
    (void)self;
    const char *s=argc?JS_ToCString(c,argv[0]):NULL;
    if(!s) return JS_UNDEFINED;
    if(!strncmp(s,"STRESS_OOM",10)) oom_lines++;
    else if(!strncmp(s,"STRESS_FAIL",11)) { fail_lines++; printf("R5SIM %s\n",s); }
    else if(!strncmp(s,"STRESS f=",9)) printf("R5SIM %s\n",s);
    JS_FreeCString(c,s);
    return JS_UNDEFINED;
}
static JSValue js_nop(JSContext *c,JSValueConst self,int argc,JSValueConst *argv) {
    (void)c;(void)self;(void)argc;(void)argv; return JS_UNDEFINED;
}

static void drain(void) {
    JSContext *c;
    while(JS_ExecutePendingJob(rt,&c)>0) {}
    pocket_kasane_end_turn();
}
static bool eval(const char *source,size_t len,const char *name) {
    JSValue v=JS_Eval(ctx,source,len,name,JS_EVAL_TYPE_GLOBAL);
    bool ok=!JS_IsException(v);
    if(!ok) {
        JSValue e=JS_GetException(ctx);
        const char *t=JS_ToCString(ctx,e);
        printf("R5SIM %s threw: %s\n",name,t?t:"?");
        if(t) JS_FreeCString(ctx,t);
        JS_FreeValue(ctx,e);
    }
    JS_FreeValue(ctx,v);
    drain();
    return ok;
}

static uint16_t *get_strip(void *o) { (void)o; return strip_pixels; }
static ksn_result send_strip(void *o,uint16_t y,uint16_t rows,const uint16_t *pixels) {
    (void)o;(void)y;(void)rows;(void)pixels; return KSN_OK;
}
static bool present(void) {
    if(!pocket_kasane_needs_present()) return true;
    ksn_display_port port={.strip=get_strip,.present=send_strip,
                           .width=240,.height=135,.strip_rows=8,.text=&ksn_font_port};
    ksn_render_stats stats;
    return pocket_kasane_present(&port,&stats)==KSN_OK;
}

// tools/test_stress_app.c's pocket.fs stub, without its fault-injection hooks.
static const char FS_STUB[]=
    "globalThis.console={log:globalThis.__log};"
    "globalThis.pocket={kasane:globalThis.kasane,fs:{open:async function(){let n=0;"
    "return{read:async function(m){__readStarted();let c=++n>3?null:new Uint8Array(m);"
    "__readFinished();return c},close:function(){}}}},memory:globalThis.memory};";

void app_main(void) {
    rt=JS_NewRuntime();
    ctx=JS_NewContext(rt); host_capabilities_clear();
    pocket_kasane_install(ctx,NULL);
    // Rendering only has to consume each submission; a 16x8 viewport keeps the
    // scalar renderer from filling the trace with pixels.
    pocket_kasane_set_viewport(0,0,16,8);
    pocket_memory_install(ctx,NULL);
    JSValue g=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,g,"__log",JS_NewCFunction(ctx,js_log,"log",1));
    JS_SetPropertyStr(ctx,g,"__readStarted",JS_NewCFunction(ctx,js_nop,"readStarted",0));
    JS_SetPropertyStr(ctx,g,"__readFinished",JS_NewCFunction(ctx,js_nop,"readFinished",0));
    eval(FS_STUB,strlen(FS_STUB),"stub.js");
    bool ok=eval(stress_js_start,(size_t)(stress_js_end-stress_js_start-1),"stress.js")&&present();
    printf("R5SIM eval %s\n",ok?"ok":"FAIL");
    // The device's arrangement (main/app_session.c, pocketjs_guest_create).
    JS_SetMemoryLimit(rt,160*1024);
    JS_SetGCThreshold(rt,80*1024);
    JSValue frame=JS_GetPropertyStr(ctx,g,"frame");
    unsigned bad_present=0;
    for(unsigned t=1;ok&&t<=R5SIM_FRAMES;t++) {
        JSValue b=JS_NewInt32(ctx,0);
        r5_turn_begin(t);
        JSValue r=JS_Call(ctx,frame,JS_UNDEFINED,1,&b);
        if(JS_IsException(r)) JS_FreeValue(ctx,JS_GetException(ctx));
        JS_FreeValue(ctx,r);
        drain();
        r5_turn_end(t);
        JSOOMCanary canary={0};
        JS_TakeOOMCanary(rt,&canary);
        if(canary.count) pocket_memory_oom(&canary,(uint64_t)t*33333u);
        size_t used=0,limit=0;
        JS_GetMemoryCounters(rt,&used,&limit);
        pocket_memory_sample((uint64_t)t*33333u,used,limit,false,0,0);
        pocket_memory_pump(false);
        if(!present()) bad_present++;
    }
    JS_FreeValue(ctx,frame);
    JS_FreeValue(ctx,g);
    printf("R5SIM done frames=%u oom=%u fail=%u bad_present=%u\n",
           (unsigned)R5SIM_FRAMES,oom_lines,fail_lines,bad_present);
}
