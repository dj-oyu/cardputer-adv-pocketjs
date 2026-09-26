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
#include "pocket_memory.h"
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

/* STRESS_ALLOC_TRACE=<file> (docs/vm/allocator-cost.md, R3's design): the
 * guest allocator's calls as text -- "m <usable> <id>", "f <id>",
 * "r <old id> <new id> <usable>", "F <frame>" -- with the device's tlsf
 * rounding (4 B, 12 B minimum) as the usable size, so a -m32 build's trace
 * has the device's sizes. Ids are sequence numbers, not addresses. */
static FILE *alloc_trace;
typedef struct { size_t usable; uint32_t id; } trace_hdr;
static uint32_t trace_next_id=1;
static size_t trace_usable(size_t n){ n=(n+3)&~(size_t)3; return n<12?12:n; }
static void *tr_malloc(void *o,size_t n){
    (void)o; if(!n) return NULL;
    trace_hdr *h=malloc(sizeof(trace_hdr)+trace_usable(n)); if(!h) return NULL;
    h->usable=trace_usable(n); h->id=trace_next_id++;
    fprintf(alloc_trace,"m %zu %u\n",h->usable,h->id); return h+1;
}
static void tr_free(void *o,void *p){
    (void)o; if(!p) return; trace_hdr *h=(trace_hdr *)p-1;
    fprintf(alloc_trace,"f %u\n",h->id); free(h);
}
static void *tr_calloc(void *o,size_t c,size_t n){
    void *p=tr_malloc(o,c*n); if(p) memset(p,0,c*n); return p;
}
static size_t tr_usable(const void *p){ return p?((const trace_hdr *)p-1)->usable:0; }
static void *tr_realloc(void *o,void *p,size_t n){
    (void)o; if(!p) return tr_malloc(o,n);
    if(!n){ tr_free(o,p); return NULL; }
    trace_hdr *h=(trace_hdr *)p-1; uint32_t old=h->id;
    trace_hdr *q=realloc(h,sizeof(trace_hdr)+trace_usable(n)); if(!q) return NULL;
    q->usable=trace_usable(n); q->id=trace_next_id++;
    fprintf(alloc_trace,"r %u %u %zu\n",old,q->id,q->usable); return q+1;
}
static const JSMallocFunctions TRACE_MF={tr_calloc,tr_malloc,tr_free,tr_realloc,tr_usable};
static JSContext *ctx;
static uint16_t strip_pixels[240*8];
static uint16_t panel[240*135];
static unsigned lines_ready,lines_native,lines_oom,lines_suspect,lines_fail,lines_stat,exceptions;
static unsigned bad_suspect;
static unsigned bad_command_count,bad_native;
static unsigned pressure_events, pressure_trims;
static unsigned reads_started,reads_finished,reads_at_first_oom,unhandled_rejections;
static unsigned dispatch_faults,reads_at_dispatch_fault,bad_fault_reads;

static JSValue js_read_started(JSContext *c,JSValueConst self,int argc,JSValueConst *argv) {
    (void)c;(void)self;(void)argc;(void)argv; reads_started++; return JS_UNDEFINED;
}
static JSValue js_read_finished(JSContext *c,JSValueConst self,int argc,JSValueConst *argv) {
    (void)c;(void)self;(void)argc;(void)argv; reads_finished++; return JS_UNDEFINED;
}
static JSValue js_dispatch_fault(JSContext *c,JSValueConst self,int argc,JSValueConst *argv) {
    (void)c;(void)self;(void)argc;(void)argv;
    dispatch_faults++;
    reads_at_dispatch_fault=reads_started;
    if(reads_started!=reads_finished)bad_fault_reads++;
    return JS_UNDEFINED;
}
static void promise_rejection(JSContext *c,JSValueConst promise,JSValueConst reason,bool handled,void *opaque) {
    (void)c;(void)promise;(void)reason;(void)opaque;
    if(handled) { if(unhandled_rejections)unhandled_rejections--; }
    else unhandled_rejections++;
}

static JSValue js_log(JSContext *c,JSValueConst self,int argc,JSValueConst *argv) {
    (void)self;
    const char *s=argc?JS_ToCString(c,argv[0]):NULL;
    if(!s) return JS_UNDEFINED;
    if(!strncmp(s,"STRESS_READY",12)) lines_ready++;
    else if(!strncmp(s,"STRESS_NATIVE",13)) {
        lines_native++;
        if(strstr(s,"STRESS_NATIVE NG"))bad_native++;
    }
    else if(!strncmp(s,"STRESS_OOM",10)) {
        if(!lines_oom)reads_at_first_oom=reads_started;
        lines_oom++;
    }
    else if(!strncmp(s,"STRESS_SUSPECT",14)) lines_suspect++;
    else if(!strncmp(s,"STRESS_FAIL",11)) { lines_fail++; printf("  %s\n",s); }
    else if(!strncmp(s,"STRESS f=",9)) {
        unsigned commands=0;const char *field=strstr(s," cmds=");
        if(!field||sscanf(field," cmds=%u",&commands)!=1||commands!=75)bad_command_count++;
        field=strstr(s," pe=");if(field)sscanf(field," pe=%u",&pressure_events);
        field=strstr(s," trim=");if(field)sscanf(field," trim=%u",&pressure_trims);
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
    "globalThis.pocket={kasane:globalThis.kasane,fs:{open:async function(){"
    "if(globalThis.__holdRead){__holdRead=false;return new Promise(()=>{})}let n=0;"
    "return{read:async function(m){"
    "if(globalThis.__holdRead){__holdRead=false;return new Promise(()=>{})}"
    "__readStarted();let c=++n>3?null:new Uint8Array(m);"
    "__readFinished();return c},close:function(){}}}},memory:globalThis.memory};";

int main(int argc,char **argv) {
    const char *gradient_arm=getenv("KSN_VERTICAL_GRAD_PIE");
    const bool inject_dispatch_fault=getenv("STRESS_DISPATCH_FAULT")!=NULL;
    if(gradient_arm)g_ksn_vertical_gradient_pie=strcmp(gradient_arm,"0")!=0;
    uint32_t panel_digest=2166136261u;
    const char *path=argc>1?argv[1]:"apps/stress/stress.js";
    FILE *f=fopen(path,"rb");
    if(!f){printf("cannot open %s\n",path);return 2;}
    static char src[1<<15]; size_t n=fread(src,1,sizeof src-1,f); fclose(f); src[n]=0;
    const char *trace_path=getenv("STRESS_ALLOC_TRACE");
    if(trace_path&&(alloc_trace=fopen(trace_path,"w"))) rt=JS_NewRuntime2(&TRACE_MF,NULL);
    else rt=JS_NewRuntime();
    ctx=JS_NewContext(rt); host_capabilities_clear();
    JS_SetHostPromiseRejectionTracker(rt,promise_rejection,NULL);
    pocket_kasane_install(ctx,NULL);
    pocket_memory_install(ctx,NULL);
    JSValue g=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,g,"__log",JS_NewCFunction(ctx,js_log,"log",1));
    JS_SetPropertyStr(ctx,g,"__readStarted",JS_NewCFunction(ctx,js_read_started,"readStarted",0));
    JS_SetPropertyStr(ctx,g,"__readFinished",JS_NewCFunction(ctx,js_read_finished,"readFinished",0));
    JS_SetPropertyStr(ctx,g,"__dispatchFault",JS_NewCFunction(ctx,js_dispatch_fault,"dispatchFault",0));
    JS_FreeValue(ctx,g);
    eval(FS_STUB,strlen(FS_STUB),"stub.js");
    bool ok=eval(src,n,path)&&present();
    printf("%s eval + first present\n",ok?"ok  ":"FAIL");
    // A 64-bit host spends more per object than the device, so the cap is
    // set above the device's 160 KiB; L3 still has to reach it.
    JS_SetMemoryLimit(rt,640*1024);
    // Profiling knobs (tools/vmtest/prof/stress_prof.sh): STRESS_HEAP_LIMIT
    // sets the device's arrangement instead (the limit, and the collector's
    // first threshold at half of it, as pocketjs_guest_create does -- right
    // for a -m32 build, whose objects are the device's size); STRESS_FRAMES
    // stops early (the verdict then no longer applies).
    const char *heap_env=getenv("STRESS_HEAP_LIMIT"),*frames_env=getenv("STRESS_FRAMES");
    if(heap_env) {
        size_t limit=(size_t)strtoul(heap_env,NULL,0);
        JS_SetMemoryLimit(rt,limit);
        if(limit/2<JS_GetGCThreshold(rt)) JS_SetGCThreshold(rt,limit/2);
    }
    unsigned frames=frames_env?(unsigned)strtoul(frames_env,NULL,0):900u;
    unsigned bad_present=0;
    for(unsigned t=1;ok&&t<=frames;t++) {
        if(inject_dispatch_fault&&t==601) {
            const char *fault="globalThis.__holdRead=true;"
                "(function(){const original=Promise.prototype.catch;"
                "Promise.prototype.catch=function(handler){Promise.prototype.catch=original;"
                "__dispatchFault();throw null}})();";
            ok=eval(fault,strlen(fault),"dispatch-fault.js");
            if(!ok)break;
        }
        char call[64];
        unsigned buttons=(t==300||t==600)?0x4000u:0u;   // L1 -> L2 -> L3
        snprintf(call,sizeof call,"frame(%u)",buttons);
        unsigned suspects_before=lines_suspect;
        if(alloc_trace) fprintf(alloc_trace,"F %u\n",t);
        eval(call,strlen(call),"frame.js");
        JSOOMCanary canary={0};
        JS_TakeOOMCanary(rt,&canary);
        if(lines_suspect>suspects_before&&!canary.count)bad_suspect++;
        if(canary.count)pocket_memory_oom(&canary,(uint64_t)t*20000u);
        size_t used=0,limit=0;
        JS_GetMemoryCounters(rt,&used,&limit);
        pocket_memory_sample((uint64_t)t*20000u,used,limit,false,0,0);
        pocket_memory_pump(false);
        if(!present()) bad_present++;
        if(gradient_arm)for(size_t i=0;i<240u*135u;i++){
            panel_digest^=panel[i];panel_digest*=16777619u;
        }
        // STRESS_PPM=<prefix>: the panel as P6 at a few frames, for looking at;
        // STRESS_PPM_AT="239,240,241" picks the frames (default 90,240,420).
        const char *ppm=getenv("STRESS_PPM"),*at=getenv("STRESS_PPM_AT");
        char want[16]; snprintf(want,sizeof want,",%u,",t);
        char list[128]; snprintf(list,sizeof list,",%s,",at?at:"90,240,420");
        if(ppm&&strstr(list,want)) {
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
    printf("frames 900: exceptions=%u fails=%u oom=%u suspects=%u bad_suspect=%u pressure_events=%u pressure_trims=%u stats=%u ready=%u bad_present=%u bad_cmds=%u bad_native=%u reads=%u/%u first_oom_reads=%u unhandled=%u dispatch_faults=%u fault_reads=%u\n",
           exceptions,lines_fail,lines_oom,lines_suspect,bad_suspect,pressure_events,pressure_trims,lines_stat,lines_ready,bad_present,bad_command_count,bad_native,
           reads_started,reads_finished,reads_at_first_oom,unhandled_rejections,
           dispatch_faults,reads_at_dispatch_fault);
    if(gradient_arm)printf("gradient arm=%d panel_digest=%08x\n",
                           g_ksn_vertical_gradient_pie,panel_digest);
    bool pass=ok&&!exceptions&&!lines_fail&&!bad_suspect&&lines_ready==1&&lines_native==1&&
              lines_oom>0&&lines_stat==15&&!bad_present&&!bad_command_count&&!bad_native&&
              pressure_events>0&&pressure_trims>0&&
              reads_started==reads_finished&&reads_started>reads_at_first_oom&&
              !unhandled_rejections&&
              (!inject_dispatch_fault||(dispatch_faults==1&&!bad_fault_reads&&
                                        reads_started>reads_at_dispatch_fault));
    pocket_memory_reset();pocket_kasane_reset(); JS_FreeContext(ctx); JS_FreeRuntime(rt);
    printf("%s\n",pass?"STRESS_HOST PASS":"STRESS_HOST FAIL");
    return pass?0:1;
}
