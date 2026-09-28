/* apps/kasane/proc_megademo.js as the firmware runs it, on the host: the real
 * QuickJS, the real pocket.kasane (view, procedural, grid resize, pixel) and
 * the real renderer presenting into a 240x135 panel after every frame().
 *
 * What it proves before a flash (no device, no serial port):
 *  - two full loops of all six scenes at the default tier, plus tier changes,
 *    scene skips and the Enter zoom, with no exception and no DEGRADE;
 *  - the dynamic plan loader: per-frame registrations, live plans never above
 *    32, the old and new sets live together before a switch, and the
 *    cumulative count per loop above 32 (read by wrapping register/unregister
 *    in JS before the app is evaluated);
 *  - every view primitive the app calls is accepted by the real validator
 *    (80 APP commands, references, text bytes, rotation, groups, cache);
 *  - the guest heap after evaluation and its peak over the run, compared by
 *    the runner with the pre-change app built the same way.
 * With MEGA_PPM=<dir> the composited panel is written at selected frames.
 *
 *   python3 tools/kasane_contract/run_megademo_app_host.py
 */
#include "pocket_kasane.h"
#include "pocket_proc.h"
#include "pocket_video.h"
#include "system/sys_device.h"
#include "ui/kasane/ksn_runtime.h"
#include "ui/kasane/ksn_render.h"
#include "text/ksn_font.h"
#include "sound.h"
#include "pocket_av.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The guest allocator as the device charges it: every block counts its
 * TLSF length, the request rounded up to 4 B with a 12 B minimum (the
 * firmware reads heap_caps_get_allocated_size(); docs/vm/backlog.md #8/#9),
 * not glibc's coarser chunks. QuickJS sees that length as the usable size,
 * so JS_SetMemoryLimit() and malloc_size mean what they mean on the device.
 * Blocks are allocated at that length, since QuickJS may use the slack.
 * The current and peak totals also give the peak inside one JS_Eval. */
typedef struct { size_t n; size_t pad[3]; } pk_hdr;   /* keeps 16 B alignment */
static size_t cur_bytes,peak_bytes;
static size_t tlsf_len(size_t n){n=(n+3)&~(size_t)3;return n<12?12:n;}
static void pk_count(size_t add,size_t sub){cur_bytes+=add;cur_bytes-=sub;if(cur_bytes>peak_bytes)peak_bytes=cur_bytes;}
static void *pk_malloc(void *o,size_t n){(void)o;if(!n)return NULL;
    pk_hdr *h=malloc(sizeof *h+tlsf_len(n));if(!h)return NULL;h->n=n;pk_count(tlsf_len(n),0);return h+1;}
static void *pk_calloc(void *o,size_t c,size_t n){void *p=pk_malloc(o,c*n);if(p)memset(p,0,c*n);return p;}
static void pk_free(void *o,void *p){(void)o;if(!p)return;pk_hdr *h=(pk_hdr *)p-1;pk_count(0,tlsf_len(h->n));free(h);}
static void *pk_realloc(void *o,void *p,size_t n){
    if(!p)return pk_malloc(o,n);
    if(!n){pk_free(o,p);return NULL;}
    pk_hdr *h=(pk_hdr *)p-1;size_t old=h->n;
    pk_hdr *q=realloc(h,sizeof *q+tlsf_len(n));if(!q)return NULL;
    q->n=n;pk_count(tlsf_len(n),tlsf_len(old));return q+1;}
static size_t pk_usable(const void *p){return p?tlsf_len(((const pk_hdr *)p-1)->n):0;}
static const JSMallocFunctions PEAK_MF={pk_calloc,pk_malloc,pk_free,pk_realloc,pk_usable};

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
/* The app does not use pocket.kasane.video (it needs a streamed source). */
esp_err_t pocket_video_install(JSContext *ctx,JSValueConst kasane){(void)ctx;(void)kasane;return ESP_OK;}
void pocket_video_reset(void){}
bool pocket_video_pending(void){return false;}
void pocket_video_present_result(ksn_result result){(void)result;}
/* grid.fold/index/view load this lazily; the app never calls them. */
const char grid_fold_js_start[] asm("_binary_grid_fold_js_start") = "globalThis.gridFold={};";
const char grid_fold_js_end[] asm("_binary_grid_fold_js_end") = "";

static JSRuntime *rt;
static JSContext *ctx;
static uint16_t strip_pixels[240*8],panel[240*135];
static unsigned exceptions,degrades,scene_lines,present_failures;
static char scene_log[4096];

static JSValue js_log(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *s=argc?JS_ToCString(c,argv[0]):NULL;
    if(!s)return JS_UNDEFINED;
    if(!strncmp(s,"MEGADEMO SCENE ",15)){
        scene_lines++;
        const char *name=s+15,*sp=strchr(name,' ');
        size_t n=sp?(size_t)(sp-name):strlen(name);
        if(strlen(scene_log)+n+2<sizeof scene_log){strncat(scene_log,name,n);strcat(scene_log," ");}
    }
    if(!strncmp(s,"MEGADEMO DEGRADE",16))degrades++;
    if(getenv("MEGA_LOG")||strncmp(s,"MEGADEMO SCENE",14))printf("  %s\n",s);
    JS_FreeCString(c,s);
    return JS_UNDEFINED;
}
static bool eval(const char *source,size_t len,const char *name){
    JSValue v=JS_Eval(ctx,source,len,name,JS_EVAL_TYPE_GLOBAL);
    bool ok=!JS_IsException(v);
    if(!ok){
        JSValue e=JS_GetException(ctx);
        const char *t=JS_ToCString(ctx,e);
        printf("  %s threw: %s\n",name,t?t:"?");
        if(t)JS_FreeCString(ctx,t);
        JSValue st=JS_GetPropertyStr(ctx,e,"stack");
        const char *s=JS_IsString(st)?JS_ToCString(ctx,st):NULL;
        if(s){printf("%s",s);JS_FreeCString(ctx,s);}
        JS_FreeValue(ctx,st);JS_FreeValue(ctx,e);
        exceptions++;
    }
    JS_FreeValue(ctx,v);
    JSContext *c;
    while(JS_ExecutePendingJob(rt,&c)>0){}
    pocket_kasane_end_turn();
    return ok;
}
static uint16_t *get_strip(void *o){(void)o;return strip_pixels;}
static ksn_result send_strip(void *o,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)o;memcpy(panel+y*240,pixels,rows*240*sizeof *pixels);return KSN_OK;
}
static void present(uint64_t now_us){
    pocket_kasane_set_animation_time(now_us);
    (void)pocket_kasane_advance(now_us);
    ksn_display_port port={.strip=get_strip,.present=send_strip,
                           .width=240,.height=135,.strip_rows=8,.text=&ksn_font_port};
    ksn_render_stats stats;
    ksn_result r=pocket_kasane_present(&port,&stats);
    if(r!=KSN_OK)present_failures++;
    else pocket_kasane_animations_presented(now_us);
}
static void ppm(const char *dir,unsigned tick){
    char name[512];snprintf(name,sizeof name,"%s/app_%04u.ppm",dir,tick);
    FILE *o=fopen(name,"wb");if(!o)return;
    fprintf(o,"P6\n240 135\n255\n");
    for(size_t i=0;i<240*135;i++){
        uint16_t p=panel[i];
        unsigned char rgb[3]={(unsigned char)((((p>>11)&31u)*255u+15u)/31u),
            (unsigned char)((((p>>5)&63u)*255u+31u)/63u),(unsigned char)(((p&31u)*255u+15u)/31u)};
        fwrite(rgb,1,3,o);
    }
    fclose(o);
}
static double num(const char *expr){
    JSValue v=JS_Eval(ctx,expr,strlen(expr),"probe.js",JS_EVAL_TYPE_GLOBAL);
    double d=-1;JS_ToFloat64(ctx,&d,v);JS_FreeValue(ctx,v);return d;
}
/* malloc_size is what glibc hands out (its rounding is coarser than the
 * device's TLSF); memory_used_size is QuickJS's own count of requested bytes
 * plus 8 per block, the closer figure for the device. Both are reported. */
static size_t heap_req_peak;
static size_t heap_used(void){
    JSMemoryUsage m;JS_ComputeMemoryUsage(rt,&m);
    if((size_t)m.memory_used_size>heap_req_peak)heap_req_peak=(size_t)m.memory_used_size;
    return (size_t)m.malloc_size;
}
static size_t heap_req(void){
    JSMemoryUsage m;JS_ComputeMemoryUsage(rt,&m);return (size_t)m.memory_used_size;
}

/* Counts live, peak and cumulative plans and host time in register(),
 * without changing what the app sees. pocket.memory.info() stands in for the
 * device's sampled internal free heap, which gates the app's prefetch: a
 * line fitted to the device's turn-boundary values (31 KB free with no plans
 * down to ~15 KB with ZENITH's 16, docs/kasane/megademo-device-limits.md). */
static const char PRELUDE[]=
    "globalThis.console={log:globalThis.__log};"
    "globalThis.pocket={kasane:globalThis.kasane,memory:{info(){"
    "return {internalFreeBytes:31000-1000*globalThis.__mega.live}}}};"
    "(function(){const P=pocket.kasane.procedural,R=P.register,U=P.unregister,D=P.draw;"
    "const M=globalThis.__mega={live:0,peak:0,total:0,frameReg:0,maxFrameReg:0,"
    "regNs:0,frameDrawNs:0,unreg:0,frameUnreg:0};"
    "P.register=function(){const t=__now();const h=R.apply(P,arguments);"
    "const d=__now()-t;M.regNs+=d;M.frameRegNs=(M.frameRegNs||0)+d;"
    "M.live++;M.total++;M.frameReg++;if(M.live>M.peak)M.peak=M.live;return h};"
    "P.unregister=function(h){U.call(P,h);M.live--;M.unreg++;M.frameUnreg++};"
    "P.draw=function(h,i){const t=__now();D.call(P,h,i);M.frameDrawNs+=__now()-t};"
    "})();";

static JSValue js_now(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;
    struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
    return JS_NewFloat64(c,(double)ts.tv_sec*1e9+(double)ts.tv_nsec);
}

int main(int argc,char **argv){
    const char *path=argc>1?argv[1]:"apps/kasane/proc_megademo.js";
    const bool baseline=argc>2&&!strcmp(argv[2],"--baseline");
    const char *ppm_dir=getenv("MEGA_PPM");
    FILE *f=fopen(path,"rb");
    if(!f){printf("cannot open %s\n",path);return 2;}
    static char src[1<<16];size_t n=fread(src,1,sizeof src-1,f);fclose(f);src[n]=0;
    rt=JS_NewRuntime2(&PEAK_MF,NULL);ctx=JS_NewContext(rt);host_capabilities_clear();
    /* MEGA_HEAP_LIMIT=163840 reproduces pocketjs_guest_create(): the limit
     * and the collector's first threshold at half of it. With glibc's
     * coarser rounding this is stricter than the device's TLSF. */
    const char *limit_env=getenv("MEGA_HEAP_LIMIT");
    if(limit_env){
        size_t limit=(size_t)strtoul(limit_env,NULL,0);
        JS_SetMemoryLimit(rt,limit);
        if(limit/2<JS_GetGCThreshold(rt))JS_SetGCThreshold(rt,limit/2);
    }
    pocket_kasane_install(ctx,NULL);
    JSValue g=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,g,"__log",JS_NewCFunction(ctx,js_log,"log",1));
    JS_SetPropertyStr(ctx,g,"__now",JS_NewCFunction(ctx,js_now,"now",0));
    JS_FreeValue(ctx,g);
    JS_RunGC(rt);
    const size_t heap_before=heap_used();
    eval(PRELUDE,strlen(PRELUDE),"prelude.js");
    JS_RunGC(rt);
    const size_t heap_prelude=heap_used();
    peak_bytes=cur_bytes;const size_t eval_start=cur_bytes;
    bool ok=eval(src,n,path);
    const size_t eval_peak=peak_bytes;
    present(0);
    JS_RunGC(rt);
    const size_t heap_after_eval=heap_used(),req_after_eval=heap_req();size_t req_live_peak=req_after_eval;
    if(getenv("MEGA_MEMDUMP")){JSMemoryUsage m;JS_ComputeMemoryUsage(rt,&m);JS_DumpMemoryUsage(stdout,&m,rt);}
    printf("%s eval + first present; source %zu B\n",ok?"ok  ":"FAIL",n);
    /* Buttons: two plain loops, then Enter zoom out/in inside Act II, a tier
     * down/up, and a scene skip in each direction. */
    const unsigned loop=baseline?48:(unsigned)num("procMegademo.len.reduce((a,b)=>a+b,0)");
    const char *frames_env=getenv("MEGA_FRAMES");
    const unsigned frames=frames_env?(unsigned)strtoul(frames_env,NULL,0):baseline?200:loop*2+400;
    size_t heap_peak=0,heap_live_peak=0;unsigned max_live_seen=0,coexist_frames=0,max_frame_reg=0;
    unsigned long long reg_ns_max=0,draw_ns_max=0;
    for(unsigned t=0;ok&&t<frames;t++){
        unsigned b=0;
        if(!baseline){
            unsigned u=t>=loop*2?t-loop*2:0;
            if(t>=loop*2){
                if(u==60||u==150)b=0x4000;            /* zoom out, then back */
                if(u==200)b=0x40;                     /* tier down */
                if(u==260)b=0x10;                     /* tier up */
                if(u==300)b=0x20;                     /* next scene */
                if(u==340)b=0x80;                     /* previous scene */
            }
        }else if(t==60||t==120)b=0x4000;
        char call[160];snprintf(call,sizeof call,"__mega.frameReg=0;__mega.frameUnreg=0;__mega.frameRegNs=0;__mega.frameDrawNs=0;frame(%u)",b);
        eval(call,strlen(call),"frame.js");
        present((uint64_t)t*33333u);
        if(!baseline){
            unsigned live=(unsigned)num("__mega.live"),reg=(unsigned)num("__mega.frameReg");
            if(live>max_live_seen)max_live_seen=live;
            unsigned long long rn=(unsigned long long)num("__mega.frameRegNs||0");
            unsigned long long dn=(unsigned long long)num("__mega.frameDrawNs");
            if(rn>reg_ns_max)reg_ns_max=rn;
            if(dn>draw_ns_max)draw_ns_max=dn;
            if(reg>max_frame_reg)max_frame_reg=reg;
            /* The next scene's plans registered while the running scene's
             * are still live: old and new sets coexist in this frame. */
            if(reg&&!num("__mega.frameUnreg"))coexist_frames++;
        }
        /* Peak with garbage (the collector's own schedule), then the live
         * heap after a full collection: the part the app actually keeps. */
        size_t h=heap_used();if(h>heap_peak)heap_peak=h;
        JS_RunGC(rt);
        h=heap_used();if(h>heap_live_peak)heap_live_peak=h;{size_t r=heap_req();if(r>req_live_peak)req_live_peak=r;}
        if(getenv("MEGA_HEAPTRACE")&&!(t%8))
            printf("heap t=%u live=%zu ...%s\n",t,h,strlen(scene_log)>12?scene_log+strlen(scene_log)-12:scene_log);
        if(ppm_dir){
            /* NEWS; TWIST iris and twist; ZENITH (HUD, barrel roll); LIMIT; the
             * zoom into the Act II monitor set (796..); MID tier (936..). */
            static const unsigned picks[]={10,52,100,150,190,220,250,290,330,360,
                                           820,850,880,960,1000,1040};
            for(unsigned i=0;i<sizeof picks/sizeof picks[0];i++)if(picks[i]==t)ppm(ppm_dir,t);
        }
    }
    if(!baseline){
        unsigned peak=(unsigned)num("__mega.peak"),total=(unsigned)num("__mega.total");
        unsigned maxreg=max_frame_reg;
        printf("plans: peak live %u (limit 32), registered %u over %u frames (%.1f loops), "
               "unregistered %u, most registrations in one frame %u, frames with two sets live %u\n",
               peak,total,frames,(double)frames/loop,(unsigned)num("__mega.unreg"),maxreg,coexist_frames);
        printf("host time: max register() per frame %.3f ms, max draw() per frame %.3f ms "
               "(host CPU, not device time)\n",reg_ns_max/1e6,draw_ns_max/1e6);
        printf("scenes: %s\n",scene_log);
        ok=ok&&peak<=32&&total>32&&coexist_frames>0&&!degrades;
    }
    printf("guest heap (malloc_size = TLSF model, %zu-bit host): runtime %zu, after prelude %zu, after eval %zu (+%zu), "
           "live peak after GC %zu (+%zu), peak before GC %zu (+%zu)\n",sizeof(void *)*8,
           heap_before,heap_prelude,heap_after_eval,heap_after_eval-heap_prelude,
           heap_live_peak,heap_live_peak-heap_prelude,heap_peak,heap_peak-heap_prelude);
    printf("guest heap (TLSF model): before eval %zu, peak during eval %zu "
           "(the compile), whole-run peak %zu\n",eval_start,eval_peak,peak_bytes);
    printf("guest heap (QuickJS memory_used_size): after eval %zu, live peak after GC %zu, "
           "peak before GC %zu; device limit 163840 (main/app_session.c heap_limit)\n",
           req_after_eval,req_live_peak,heap_req_peak);
    printf("frames %u: exceptions=%u degrades=%u present_failures=%u scene_lines=%u\n",
           frames,exceptions,degrades,present_failures,scene_lines);
    ok=ok&&!exceptions&&!present_failures;
    pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
    printf("%s\n",ok?"MEGADEMO_HOST PASS":"MEGADEMO_HOST FAIL");
    return ok?0:1;
}
