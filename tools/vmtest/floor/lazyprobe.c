// Runs the shipped JS apps on the host against the real QuickJS and the real
// pocket.kasane, with small stand-ins for the other pocket.* surfaces, and
// reports how often a property lookup lands on a builtin object and misses
// there (what a "no index for builtins" scheme would pay a table search for).
#include "pocket_kasane.h"
#include "pocket_av.h"
#include "system/sys_device.h"
#include "ui/kasane/ksn_runtime.h"
#include "text/ksn_font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void host_capabilities_clear(void);
int32_t pocket_av_ui_current_player(void){return 0;}
bool pocket_av_ui_read(int32_t id,pocket_av_ui_snapshot *out){(void)id;(void)out;return false;}
bool sys_device_clock_read(sys_clock_state *out){(void)out;return false;}
typedef void (*sound_stream_observer_fn)(int32_t,uint32_t,uint32_t,bool);
void sound_stream_set_observer(sound_stream_observer_fn o){(void)o;}
void sound_stream_set_observer_interval(sound_stream_observer_fn o,uint32_t n){(void)o;(void)n;}
void *__real_calloc(size_t count,size_t size);
void __real_free(void *ptr);
void __wrap_free(void *ptr) { __real_free(ptr); }
void *__wrap_calloc(size_t count,size_t size) { return __real_calloc(count,size); }

void lp_mark(JSRuntime *rt);
void lp_enable(int on);
extern unsigned long lp_miss, lp_first, lp_hit;

static JSRuntime *rt;
static JSContext *ctx;
static uint16_t strip_pixels[240*8];

static bool eval(const char *source,const char *name) {
    JSValue v=JS_Eval(ctx,source,strlen(source),name,JS_EVAL_TYPE_GLOBAL);
    bool ok=!JS_IsException(v);
    if(!ok) {
        JSValue e=JS_GetException(ctx);
        const char *t=JS_ToCString(ctx,e);
        printf("    %s threw: %s\n",name,t?t:"?");
        if(t)JS_FreeCString(ctx,t);
        JS_FreeValue(ctx,e);
    }
    JS_FreeValue(ctx,v);
    for(;;) { JSContext *c; if(JS_ExecutePendingJob(rt,&c)<=0) break; }
    pocket_kasane_end_turn();
    return ok;
}
static uint16_t *get_strip(void *o) { (void)o; return strip_pixels; }
static ksn_result send_strip(void *o,uint16_t y,uint16_t rows,const uint16_t *pixels) {
    (void)o;(void)y;(void)rows;(void)pixels; return KSN_OK;
}
static void present(void) {
    if(!pocket_kasane_needs_present()) return;
    ksn_display_port port={.strip=get_strip,.present=send_strip,
                           .width=240,.height=135,.strip_rows=8,.text=&ksn_font_port};
    ksn_render_stats stats;
    (void)pocket_kasane_present(&port,&stats);
}

static const char *STUB =
"globalThis.console={log(){},warn(){},error(){}};"
"var __act=[],__imu=null,__t=0;globalThis.__petNow=function(){return __t};"
"globalThis.pocket={kasane:globalThis.kasane,"
" input:{onAction(f){__act.push(f);return{cancel(){}}},held(){return false},"
"  text:{open(){return{cancel(){},close(){}}}}},"
" audio:{tone(){return Promise.resolve()}},"
" capabilities:{get(){return{supported:true,available:true,reason:null}}},"
" sensors:{imu:{latest(){return{accel:[0,0,1000],gyro:[0,0,0],dropped:0}},"
"  watch(o,f){__imu=f;return{cancel(){}}}}},"
" storage:{get(){return Promise.resolve(null)},set(){return Promise.resolve()}},"
" pet:{select(){return 0},rewards(){return 3},now(){return __t},"
"  clock(){return{wakeMinute:-1,minute:600,utc:false,f:0,h:10,i:0,l:0}},"
"  usage(){return{stale:false,windows:[{usedPercent:40,resetsAt:0},{usedPercent:null,resetsAt:0}]}},timer(){return null},alarm(){},wake(){}},"
" device:{info(){return{model:'host'}}},power:{onChange(){return{cancel(){}}}},"
" random:{create(s){let x=s|0;return{nextUint(){x=(x*1103515245+12345)|0;return x>>>0},next(){return 0}}}}};";

static char *slurp(const char *path) {
    FILE *f=fopen(path,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    char *b=malloc(n+1); if(fread(b,1,n,f)!=(size_t)n){fclose(f);free(b);return NULL;}
    b[n]=0; fclose(f); return b;
}

static void run(const char *path, unsigned frames) {
    char *src=slurp(path);
    if(!src){ printf("%-28s missing\n",path); return; }
    rt=JS_NewRuntime(); ctx=JS_NewContext(rt); host_capabilities_clear();
    lp_mark(rt);
    pocket_kasane_install(ctx,NULL);
    eval(STUB,"stub.js");
    lp_enable(1);
    bool ok=eval(src,path);
    unsigned long m0=lp_miss,f0=lp_first,h0=lp_hit;
    char call[512];
    for(unsigned i=0;i<frames&&ok;i++) {
        unsigned b=(i%30==15)?0x4000:0;
        snprintf(call,sizeof call,
          "__t+=33;if(__imu)__imu({accel:[%d,0,1000],gyro:[0,%d,0],dropped:0});"
          "if(%u)for(const f of __act)f({action:'accept',phase:'press'});"
          "typeof frame==='function'&&frame(%u)", (int)(i%200)-100,(int)(i%50)-25, b?1u:0u, b);
        ok=eval(call,"frame.js");
        present();
    }
    lp_enable(0);
    printf("%-26s %s startup: miss=%lu first=%lu hit=%lu | per frame (%u): miss=%.1f first=%.2f hit=%.1f\n",
           path, ok?"ok  ":"FAIL", m0, f0, h0, frames,
           (double)(lp_miss-m0)/frames, (double)(lp_first-f0)/frames, (double)(lp_hit-h0)/frames);
    pocket_kasane_reset(); JS_FreeContext(ctx); JS_FreeRuntime(rt);
    free(src);
}

int main(void) {
    const char *apps[]={"apps/hello/main.js","apps/imucal/imucal.js","apps/pet/pet.js",
                        "apps/companion/companion.js","apps/deskclock/deskclock.js",
                        "apps/kasane/demo.js"};
    for(unsigned i=0;i<sizeof apps/sizeof apps[0];i++) run(apps[i],300);
    return 0;
}
