// apps/stress/stress.js against the real QuickJS and the real pocket.kasane,
// the way the firmware runs it: one evaluation, then frame() turns with the
// job queue drained after each. pocket.fs is a stub here (its reads are JS
// Uint8Arrays, so the native-path check is only meaningful on the device --
// tools/stress_app.py). What this catches before a flash: a scene the Kasane
// validator refuses (radius, command budget, cache budget), an exception in a
// turn, and the three levels each surviving their frames. The heap is capped
// so L3 really reaches an out-of-memory and has to recover from it.
//
//   wsl -e bash -lc "cd <repo> && bash tools/build_stress_app_test.sh && /tmp/test-stress-app"
#include "pocket_kasane.h"
#include "pocket_av.h"
#include "system/sys_device.h"
#include "ui/kasane/ksn_runtime.h"
#include "ui/kasane/ksn_render.h"
#include "text/ksn_font.h"
#include "sound.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void host_capabilities_clear(void);
int32_t pocket_av_ui_current_player(void){return 0;}
bool pocket_av_ui_read(int32_t id,pocket_av_ui_snapshot *out){
    (void)id;(void)out;return false;
}
bool sys_device_clock_read(sys_clock_state *out){(void)out;return false;}
void sound_stream_set_observer(sound_stream_observer_fn o){(void)o;}
void sound_stream_set_observer_interval(sound_stream_observer_fn o,uint32_t n){(void)o;(void)n;}
void *__real_calloc(size_t count,size_t size);
void __real_free(void *ptr);
void __wrap_free(void *ptr) { __real_free(ptr); }
void *__wrap_calloc(size_t count,size_t size) { return __real_calloc(count,size); }

static JSRuntime *rt;
static JSContext *ctx;
static uint16_t strip_pixels[240*8];
static uint16_t panel[240*135];
static unsigned lines_ready,lines_native,lines_oom,lines_fail,lines_stat,exceptions;
static unsigned bad_command_count,bad_native;

static JSValue js_log(JSContext *c,JSValueConst self,int argc,JSValueConst *argv) {
    (void)self;
    const char *s=argc?JS_ToCString(c,argv[0]):NULL;
    if(!s) return JS_UNDEFINED;
    if(!strncmp(s,"STRESS_READY",12)) lines_ready++;
    else if(!strncmp(s,"STRESS_NATIVE",13)) {
        lines_native++;
        if(strstr(s,"STRESS_NATIVE NG"))bad_native++;
    }
    else if(!strncmp(s,"STRESS_OOM",10)) lines_oom++;
    else if(!strncmp(s,"STRESS_FAIL",11)) { lines_fail++; printf("  %s\n",s); }
    else if(!strncmp(s,"STRESS f=",9)) {
        unsigned commands=0;const char *field=strstr(s," cmds=");
        if(!field||sscanf(field," cmds=%u",&commands)!=1||commands!=75)bad_command_count++;
        lines_stat++;if(lines_stat%5==1)printf("  %s\n",s);
    }
    else printf("  %s\n",s);
    JS_FreeCString(c,s);
    return JS_UNDEFINED;
}

static bool eval(const char *source,size_t len,const char *name) {
    JSValue v=JS_Eval(ctx,source,len,name,JS_EVAL_TYPE_GLOBAL);
    bool ok=!JS_IsException(v);
    if(!ok) {
        JSValue e=JS_GetException(ctx);
        const char *t=JS_ToCString(ctx,e);
        printf("  %s threw: %s\n",name,t?t:"?");
        if(t)JS_FreeCString(ctx,t);
        JSValue st=JS_GetPropertyStr(ctx,e,"stack");
        const char *s=JS_IsString(st)?JS_ToCString(ctx,st):NULL;
        if(s){printf("%s",s);JS_FreeCString(ctx,s);}
        JS_FreeValue(ctx,st);
        JS_FreeValue(ctx,e);
        exceptions++;
    }
    JS_FreeValue(ctx,v);
    JSContext *c;
    while(JS_ExecutePendingJob(rt,&c)>0) {}
    pocket_kasane_end_turn();
    return ok;
}

static uint16_t *get_strip(void *o) { (void)o; return strip_pixels; }
static ksn_result send_strip(void *o,uint16_t y,uint16_t rows,const uint16_t *pixels) {
    (void)o; memcpy(panel+y*240,pixels,rows*240*sizeof(*pixels)); return KSN_OK;
}
static bool present(void) {
    if(!pocket_kasane_needs_present()) return true;
    ksn_display_port port={.strip=get_strip,.present=send_strip,
                           .width=240,.height=135,.strip_rows=8,.text=&ksn_font_port};
    ksn_render_stats stats;
    return pocket_kasane_present(&port,&stats)==KSN_OK;
}

// A pocket.fs with the one shape the app uses: open() -> {read(n), close()},
// three 1 KiB chunks then null (end of file).
static const char FS_STUB[]=
    "globalThis.console={log:globalThis.__log};"
    "globalThis.pocket={kasane:globalThis.kasane,fs:{open:async function(){let n=0;"
    "return{read:async function(m){return ++n>3?null:new Uint8Array(m)},close:function(){}}}}};";

int main(int argc,char **argv) {
    const char *gradient_arm=getenv("KSN_VERTICAL_GRAD_PIE");
    if(gradient_arm)g_ksn_vertical_gradient_pie=strcmp(gradient_arm,"0")!=0;
    uint32_t panel_digest=2166136261u;
    const char *path=argc>1?argv[1]:"apps/stress/stress.js";
    FILE *f=fopen(path,"rb");
    if(!f){printf("cannot open %s\n",path);return 2;}
    static char src[1<<15]; size_t n=fread(src,1,sizeof src-1,f); fclose(f); src[n]=0;
    rt=JS_NewRuntime(); ctx=JS_NewContext(rt); host_capabilities_clear();
    pocket_kasane_install(ctx,NULL);
    JSValue g=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,g,"__log",JS_NewCFunction(ctx,js_log,"log",1));
    JS_FreeValue(ctx,g);
    eval(FS_STUB,strlen(FS_STUB),"stub.js");
    bool ok=eval(src,n,path)&&present();
    printf("%s eval + first present\n",ok?"ok  ":"FAIL");
    // A 64-bit host spends more per object than the device, so the cap is
    // set above the device's 160 KiB; L3 still has to reach it.
    JS_SetMemoryLimit(rt,640*1024);
    unsigned bad_present=0;
    for(unsigned t=1;ok&&t<=900;t++) {
        char call[64];
        unsigned buttons=(t==300||t==600)?0x4000u:0u;   // L1 -> L2 -> L3
        snprintf(call,sizeof call,"frame(%u)",buttons);
        eval(call,strlen(call),"frame.js");
        if(!present()) bad_present++;
        if(gradient_arm)for(size_t i=0;i<240u*135u;i++){
            panel_digest^=panel[i];panel_digest*=16777619u;
        }
        // STRESS_PPM=<prefix>: the panel as P6 at a few frames, for looking at.
        const char *ppm=getenv("STRESS_PPM");
        if(ppm&&(t==90||t==240||t==420)) {
            char name[256]; snprintf(name,sizeof name,"%s-%03u.ppm",ppm,t);
            FILE *o=fopen(name,"wb");
            if(o) {
                fprintf(o,"P6\n240 135\n255\n");
                for(size_t i=0;i<240*135;i++) {
                    uint16_t p=panel[i];
                    unsigned char rgb[3]={(unsigned char)((p>>11)<<3),(unsigned char)(((p>>5)&63)<<2),
                                          (unsigned char)((p&31)<<3)};
                    fwrite(rgb,1,3,o);
                }
                fclose(o);
            }
        }
    }
    printf("frames 900: exceptions=%u fails=%u oom=%u stats=%u ready=%u bad_present=%u bad_cmds=%u bad_native=%u\n",
           exceptions,lines_fail,lines_oom,lines_stat,lines_ready,bad_present,bad_command_count,bad_native);
    if(gradient_arm)printf("gradient arm=%d panel_digest=%08x\n",
                           g_ksn_vertical_gradient_pie,panel_digest);
    bool pass=ok&&!exceptions&&!lines_fail&&lines_ready==1&&lines_native==1&&
              lines_oom>0&&lines_stat==15&&!bad_present&&!bad_command_count&&!bad_native;
    pocket_kasane_reset(); JS_FreeContext(ctx); JS_FreeRuntime(rt);
    printf("%s\n",pass?"STRESS_HOST PASS":"STRESS_HOST FAIL");
    return pass?0:1;
}
