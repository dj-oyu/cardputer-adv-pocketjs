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
#include "pocket_shape.h"
#include "pocket_kasane.h"
#include "pocket_proc.h"
#include "pocket_video.h"
#include "system/sys_device.h"
#include "ui/kasane/ksn_runtime.h"
#include "ui/kasane/ksn_render.h"
#include "text/ksn_font.h"
#include "sound.h"
#include "pocket_av.h"
#include "ksn_proc_plan.h"
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
#ifdef CHARACTER_MODEL_STUDY
#define CHARACTER_MEMORY_OBSERVER 1
/* Link wrapping is invisible to builtin-malloc optimization. These stores
 * must survive around calls that the compiler otherwise treats as pure. */
static volatile unsigned study_guest_depth,study_observer_depth;
static bool study_native_active;
#define STUDY_GUEST_ENTER (++study_guest_depth)
#define STUDY_GUEST_LEAVE (--study_guest_depth)
#else
#define STUDY_GUEST_ENTER ((void)0)
#define STUDY_GUEST_LEAVE ((void)0)
#endif
#ifdef CHARACTER_MEMORY_OBSERVER
/* Match this QuickJS build's source-level accounting policy, and verify it
 * against JS_ComputeMemoryUsage at every live observation checkpoint. */
#if defined(__APPLE__)
#define CHARACTER_MALLOC_OVERHEAD 0u
#else
#define CHARACTER_MALLOC_OVERHEAD 8u
#endif
static size_t character_memory_live_blocks,character_memory_accounted_peak;
#endif
typedef struct { size_t n; size_t pad[3]; } pk_hdr;   /* keeps 16 B alignment */
static size_t cur_bytes,peak_bytes;
static unsigned long long alloc_calls,alloc_bytes; /* MEGA_SCENESTATS: what frame() allocates */
static size_t largest_guest_request; /* requested guest allocation, not native free/largest */
static size_t tlsf_len(size_t n){n=(n+3)&~(size_t)3;return n<12?12:n;}
static void pk_count(size_t add,size_t sub){
    cur_bytes+=add;cur_bytes-=sub;if(cur_bytes>peak_bytes)peak_bytes=cur_bytes;
#ifdef CHARACTER_MEMORY_OBSERVER
    const size_t accounted=cur_bytes+CHARACTER_MALLOC_OVERHEAD*character_memory_live_blocks;
    if(accounted>character_memory_accounted_peak)character_memory_accounted_peak=accounted;
#endif
}
static void *pk_malloc(void *o,size_t n){(void)o;if(!n)return NULL;
    if(n>largest_guest_request)largest_guest_request=n;
    STUDY_GUEST_ENTER;pk_hdr *h=malloc(sizeof *h+tlsf_len(n));STUDY_GUEST_LEAVE;if(!h)return NULL;h->n=n;
#ifdef CHARACTER_MEMORY_OBSERVER
    ++character_memory_live_blocks;
#endif
    pk_count(tlsf_len(n),0);
    alloc_calls++;alloc_bytes+=tlsf_len(n);return h+1;}
static void *pk_calloc(void *o,size_t c,size_t n){void *p=pk_malloc(o,c*n);if(p)memset(p,0,c*n);return p;}
static void pk_free(void *o,void *p){(void)o;if(!p)return;pk_hdr *h=(pk_hdr *)p-1;
#ifdef CHARACTER_MEMORY_OBSERVER
    --character_memory_live_blocks;
#endif
    pk_count(0,tlsf_len(h->n));STUDY_GUEST_ENTER;free(h);STUDY_GUEST_LEAVE;
}
static void *pk_realloc(void *o,void *p,size_t n){
    if(!p)return pk_malloc(o,n);
    if(!n){pk_free(o,p);return NULL;}
    pk_hdr *h=(pk_hdr *)p-1;size_t old=h->n;
    if(n>largest_guest_request)largest_guest_request=n;
    STUDY_GUEST_ENTER;pk_hdr *q=realloc(h,sizeof *q+tlsf_len(n));STUDY_GUEST_LEAVE;if(!q)return NULL;
    q->n=n;pk_count(tlsf_len(n),tlsf_len(old));
    alloc_calls++;alloc_bytes+=tlsf_len(n);return q+1;}
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
#ifdef CHARACTER_MODEL_STUDY
void *__real_malloc(size_t size);
void *__real_realloc(void *ptr,size_t size);
/* Fixed observer registry: no observer allocation; counts requested bytes only.
 * libc-internal allocations not passing these linked symbols are outside scope. */
static struct {void *ptr;size_t size;} study_native_blocks[4096];
static volatile size_t study_native_current,study_native_peak,study_native_largest,study_native_count;
static volatile unsigned long long study_native_calls;
static bool study_native_overflow;
static bool study_native_watch(void){return study_native_active&&!study_guest_depth&&!study_observer_depth;}
static int study_native_find(void *ptr){for(unsigned i=0;i<4096;++i)if(study_native_blocks[i].ptr==ptr)return (int)i;return -1;}
static void study_native_add(void *ptr,size_t size){
    if(!ptr)return;
    int slot=study_native_find(NULL);
    if(slot<0){study_native_overflow=true;return;}
    study_native_blocks[slot].ptr=ptr;study_native_blocks[slot].size=size;
    study_native_current+=size;++study_native_count;
    if(study_native_current>study_native_peak)study_native_peak=study_native_current;
}
static void study_native_remove(int i){
    if(i<0)return;
    study_native_current-=study_native_blocks[i].size;--study_native_count;
    study_native_blocks[i].ptr=NULL;study_native_blocks[i].size=0;
}
void *__wrap_malloc(size_t size){
    const bool watch=study_native_watch();if(watch){++study_native_calls;if(size>study_native_largest)study_native_largest=size;}
    void *p=__real_malloc(size);if(watch)study_native_add(p,size);return p;
}
void *__wrap_calloc(size_t count,size_t size){
    const size_t bytes=count&&size>SIZE_MAX/count?SIZE_MAX:count*size;const bool watch=study_native_watch();
    if(watch){++study_native_calls;if(bytes>study_native_largest)study_native_largest=bytes;}
    void *p=__real_calloc(count,size);if(watch)study_native_add(p,bytes);return p;
}
void *__wrap_realloc(void *ptr,size_t size){
    const bool watch=study_native_watch();const int slot=watch&&ptr?study_native_find(ptr):-1;
    if(watch){++study_native_calls;if(size>study_native_largest)study_native_largest=size;}
    void *p=__real_realloc(ptr,size);
    if(watch&&(p||!size)){study_native_remove(slot);study_native_add(p,size);}return p;
}
void __wrap_free(void *ptr){if(ptr&&study_native_watch())study_native_remove(study_native_find(ptr));__real_free(ptr);}
static bool study_allocator_selftest(void){
    const size_t guest=cur_bytes,blocks=character_memory_live_blocks,native=study_native_current;
    const unsigned long long calls=study_native_calls;
    void *p=pk_malloc(NULL,32);
    const bool guest_ok=p&&cur_bytes==guest+32&&character_memory_live_blocks==blocks+1&&
        study_native_current==native&&study_native_calls==calls;
    pk_free(NULL,p);
    void *q=__wrap_malloc(47);
    if(q)((volatile unsigned char *)q)[0]=0xa5;
    const bool native_ok=q&&study_native_current==native+47&&study_native_calls==calls+1;
    __wrap_free(q);
    const bool released=cur_bytes==guest&&character_memory_live_blocks==blocks&&study_native_current==native;
    printf("MODEL_OBSERVER_SELFTEST guest_excluded=%s native_tracked=%s released=%s\n",
        guest_ok?"PASS":"FAIL",native_ok?"PASS":"FAIL",released?"PASS":"FAIL");
    return guest_ok&&native_ok&&released;
}
#else
void __wrap_free(void *ptr){__real_free(ptr);}
void *__wrap_calloc(size_t count,size_t size){return __real_calloc(count,size);}
#endif
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
static unsigned long long panel_bytes; /* what the panel was sent, as the device's SPI would */
static unsigned exceptions,degrades,scene_lines,present_failures;
static char scene_log[4096];
static unsigned long long st_frames[8],st_calls[8],st_bytes[8];static double st_us[8],st_usmax[8];

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
    /* The turn is over: the firmware returns the cached frame segments
     * (components/pocketjs_guest/src/guest.c, D43). */
    JS_VMStackTrim(rt);
    pocket_kasane_end_turn();
    return ok;
}
/* pocket.app.load() as main/pocket/pocket_app_load.c does it for this app:
 * apps/kasane/chunks.txt names the chunks; each loads once, as its own
 * global script, so the compile peak is per chunk (docs/vm/eval-peak.md 7). */
static struct {char name[32],path[512];bool done;} chunk[16];
static unsigned chunks;
static bool strip_debug;   /* chunks.txt `option strip-debug`, as pocket_app_load.c */
static void read_chunks(void){
    /* MEGA_CHUNKS: another chunks.txt (the runner's baseline, extracted). */
    const char *list=getenv("MEGA_CHUNKS");if(!list)list="apps/kasane/chunks.txt";
    char dir[200];snprintf(dir,sizeof dir,"%s",list);
    char *slash=strrchr(dir,'/');if(slash)*slash=0;else snprintf(dir,sizeof dir,".");
    FILE *f=fopen(list,"r");if(!f)return;
    char line[512],a[32],b[200];
    while(fgets(line,sizeof line,f)){
        char *h=strchr(line,'#');if(h)*h=0;
        if(sscanf(line,"%31s %199s",a,b)!=2||!strcmp(a,"app")||chunks==16)continue;
        if(!strcmp(a,"option")){if(!strcmp(b,"strip-debug"))strip_debug=true;continue;}
        snprintf(chunk[chunks].name,sizeof chunk[0].name,"%s",a);
        snprintf(chunk[chunks].path,sizeof chunk[0].path,"%s/%s",dir,b);
        chunks++;
    }
    fclose(f);
}
static JSValue js_load(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *name=argc?JS_ToCString(c,argv[0]):NULL;
    if(!name)return JS_EXCEPTION;
    unsigned i=0;while(i<chunks&&strcmp(chunk[i].name,name))i++;
    JS_FreeCString(c,name);
    if(i==chunks)return JS_ThrowReferenceError(c,"no chunk");
    if(chunk[i].done)return JS_FALSE;
    FILE *f=fopen(chunk[i].path,"rb");if(!f)return JS_ThrowReferenceError(c,"no file");
    static char body[1<<16];size_t n=fread(body,1,sizeof body-1,f);fclose(f);body[n]=0;
    chunk[i].done=true;
    JS_PocketSetStripDebug(c,strip_debug);
    JSValue v=JS_Eval(c,body,n,chunk[i].path,JS_EVAL_TYPE_GLOBAL);
    JS_PocketSetStripDebug(c,0);
    if(JS_IsException(v))return v;
    JS_FreeValue(c,v);
    return JS_TRUE;
}
static uint16_t *get_strip(void *o){(void)o;return strip_pixels;}
static ksn_result send_strip(void *o,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)o;memcpy(panel+y*240,pixels,rows*240*sizeof *pixels);panel_bytes+=rows*240u*2u;return KSN_OK;
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

#ifdef CHARACTER_MEMORY_OBSERVER
/* Opt-in host observer: all fields below are raw QuickJS/guest-allocator
 * measurements. Categories overlap (code is part of function size), and
 * binary_object_size means serialized bytecode, never ArrayBuffer storage.
 * JS_mallocz opaque payloads count against the guest allocator but may not
 * be separately categorized by QuickJS's object walker. No GC is run here. */
static FILE *character_memory_output;
static size_t character_memory_total_peak,character_memory_total_request,character_memory_total_accounted_peak;
static bool character_memory_failed;
static void character_memory_record(const char *phase,bool explicit_gc){
    const char *path=getenv("CHARACTER_MEMORY_STUDY");if(!path)return;
    if(!character_memory_output){
        character_memory_output=fopen(path,"w");
        if(!character_memory_output){character_memory_failed=true;return;}
    }
    ++study_observer_depth;
    JSMemoryUsage m;memset(&m,0,sizeof m);
    const unsigned long long before=alloc_calls;
    if(rt)JS_ComputeMemoryUsage(rt,&m);
    if(peak_bytes>character_memory_total_peak)character_memory_total_peak=peak_bytes;
    if(largest_guest_request>character_memory_total_request)character_memory_total_request=largest_guest_request;
    if(character_memory_accounted_peak>character_memory_total_accounted_peak)character_memory_total_accounted_peak=character_memory_accounted_peak;
    const size_t expected_accounted=cur_bytes+CHARACTER_MALLOC_OVERHEAD*character_memory_live_blocks;
    const bool accounting_valid=!rt||((size_t)m.malloc_size==expected_accounted&&(size_t)m.malloc_count==character_memory_live_blocks);
    if(!accounting_valid)character_memory_failed=true;
    FILE *f=character_memory_output;
    fprintf(f,"{\"kind\":\"HOST SIMULATION\",\"phase\":\"%s\",\"explicit_gc_before_sample\":%s,"
        "\"host_pointer_bits\":%zu,\"guest_allocator_current_tlsf_bytes\":%zu,"
        "\"guest_allocator_phase_peak_tlsf_bytes\":%zu,\"guest_allocator_total_peak_tlsf_bytes\":%zu,"
        "\"guest_allocator_phase_max_request_bytes\":%zu,\"guest_allocator_total_max_request_bytes\":%zu,"
        "\"guest_allocator_alloc_calls\":%llu,\"guest_allocator_allocated_bytes_cumulative\":%llu,"
        "\"observer_guest_alloc_calls\":%llu,\"off_guest_native_requested_bytes\":null,"
        "\"native_free_bytes\":null,\"native_largest_free_bytes\":null,\"native_stack_bytes\":null,"
        "\"fixture_static_panel_strip_bytes\":%zu,\"quickjs\":",
        phase,explicit_gc?"true":"false",sizeof(void*)*8,cur_bytes,peak_bytes,character_memory_total_peak,
        largest_guest_request,character_memory_total_request,alloc_calls,alloc_bytes,alloc_calls-before,
        sizeof panel+sizeof strip_pixels);
    /* Peaks come from every successful allocator event, not end-of-phase
     * block count. They include QuickJS's per-live-block quota overhead. */
    if(rt){
        fputc('{',f);
#define MEMORY_FIELD(name) fprintf(f,"\"" #name "\":%lld,",(long long)m.name)
        MEMORY_FIELD(malloc_size);MEMORY_FIELD(malloc_limit);MEMORY_FIELD(memory_used_size);
        MEMORY_FIELD(malloc_count);MEMORY_FIELD(memory_used_count);
        MEMORY_FIELD(atom_count);MEMORY_FIELD(atom_size);MEMORY_FIELD(str_count);MEMORY_FIELD(str_size);
        MEMORY_FIELD(obj_count);MEMORY_FIELD(obj_size);MEMORY_FIELD(prop_count);MEMORY_FIELD(prop_size);
        MEMORY_FIELD(shape_count);MEMORY_FIELD(shape_size);MEMORY_FIELD(js_func_count);MEMORY_FIELD(js_func_size);
        MEMORY_FIELD(js_func_code_size);MEMORY_FIELD(js_func_pc2line_count);MEMORY_FIELD(js_func_pc2line_size);
        MEMORY_FIELD(c_func_count);MEMORY_FIELD(array_count);MEMORY_FIELD(fast_array_count);MEMORY_FIELD(fast_array_elements);
        MEMORY_FIELD(binary_object_count);
#undef MEMORY_FIELD
        fprintf(f,"\"binary_object_size\":%lld}",(long long)m.binary_object_size);
    }else fputs("null",f);
    fprintf(f,",\"guest_allocator_live_blocks\":%zu,\"guest_quota_overhead_per_block_bytes\":%u,"
        "\"guest_accounted_phase_peak_bytes\":%zu,\"guest_accounted_total_peak_bytes\":%zu,\"accounting_formula_verified\":%s",
        character_memory_live_blocks,CHARACTER_MALLOC_OVERHEAD,character_memory_accounted_peak,
        character_memory_total_accounted_peak,accounting_valid?"true":"false");
    fprintf(f,",\"explicit_native_requested_current\":%zu,\"explicit_native_requested_peak\":%zu,\"explicit_native_largest_request\":%zu,\"explicit_native_live_blocks\":%zu,\"explicit_native_alloc_calls\":%llu,\"explicit_native_registry_overflow\":%s,\"fixture_observer_registry_bytes\":%zu",study_native_current,study_native_peak,study_native_largest,study_native_count,study_native_calls,study_native_overflow?"true":"false",sizeof study_native_blocks);
    --study_observer_depth;
    if(fputs("}\n",f)==EOF||fflush(f))character_memory_failed=true;
    peak_bytes=cur_bytes;largest_guest_request=0;character_memory_accounted_peak=expected_accounted;
}
static void character_memory_close(void){
    if(character_memory_output&&fclose(character_memory_output))character_memory_failed=true;
    character_memory_output=NULL;
}
#endif

/* Counts live, peak and cumulative plans and host time in register(),
 * without changing what the app sees. pocket.memory.info() stands in for the
 * device's sampled internal free heap, which gates the app's prefetch: a
 * line fitted to the device's turn-boundary values (31 KB free with no plans
 * down to ~15 KB with ZENITH's 16, docs/kasane/megademo-device-limits.md). */
static const char PRELUDE[]=
    "globalThis.console={log:globalThis.__log};"
    "globalThis.pocket={kasane:globalThis.kasane,hone:globalThis.hone,shapes:globalThis.shapes,ayatsuri:globalThis.ayatsuri,app:{load:globalThis.__load},memory:{info(){"
    "return {internalFreeBytes:31000-1000*globalThis.__mega.live}}}};"
    "(function(){const P=pocket.kasane.procedural,R=P.register,U=P.unregister,D=P.draw;"
    "const M=globalThis.__mega={live:0,peak:0,total:0,frameReg:0,maxFrameReg:0,"
    "regNs:0,frameDrawNs:0,unreg:0,frameUnreg:0};"
    "P.register=function(){const t=__now();const h=R.apply(P,arguments);"
    "const d=__now()-t;M.regNs+=d;M.frameRegNs=(M.frameRegNs||0)+d;"
    "M.live++;M.total++;M.frameReg++;if(M.live>M.peak)M.peak=M.live;return h};"
    "P.unregister=function(h){U.call(P,h);M.live--;M.unreg++;M.frameUnreg++};"
    "P.draw=function(h,a,b,c,d,e,f,g,i){const t=__now();D.call(P,h,a,b,c,d,e,f,g,i);M.frameDrawNs+=__now()-t};"
    "})();";

static JSValue js_now(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;
    struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
    return JS_NewFloat64(c,(double)ts.tv_sec*1e9+(double)ts.tv_nsec);
}

void vmtest_vmstack_report(JSRuntime *rt,void *out); /* quickjs-vm.c */
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
    pocket_shape_install(ctx,NULL);
    JSValue g=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,g,"__log",JS_NewCFunction(ctx,js_log,"log",1));
    JS_SetPropertyStr(ctx,g,"__now",JS_NewCFunction(ctx,js_now,"now",0));
    JS_SetPropertyStr(ctx,g,"__load",JS_NewCFunction(ctx,js_load,"load",1));
    /* The built-in plan table, as app_session.c hands it over (the runner
     * generates it from megademo_plans.js: megademo_chunks.rom_table). */
    pocket_proc_rom_plans(ksn_proc_rom_plans,ksn_proc_rom_plans_count);
    read_chunks();
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
    const unsigned loop=baseline?48:(unsigned)num("LEN.reduce((a,b)=>a+b,0)");
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
        /* MEGA_KEYS="t:buttons,t:buttons,..." replaces the script (frames to
         * picture a story beat; "0:0" for no keys). */
        const char *keys=getenv("MEGA_KEYS");
        if(keys){b=0;for(const char *c=keys;*c;){char *e;unsigned long k=strtoul(c,&e,10);
                if(e==c||*e!=':')break;
                unsigned long v=strtoul(e+1,&e,0);
                if(k==t)b=(unsigned)v;
                c=*e?e+1:e;}}
        char call[160];snprintf(call,sizeof call,"__mega.frameReg=0;__mega.frameUnreg=0;__mega.frameRegNs=0;__mega.frameDrawNs=0;frame(%u)",b);
        /* MEGA_SCENESTATS: per scene (the app's `scene`, before the call),
         * allocations and bytes frame() asks the guest heap for, and its
         * host time (plans' draws included, the present not). Loops 1-2 only. */
        const unsigned sc=t<loop*2?(unsigned)num("scene")&7:8;
        const unsigned long long a0=alloc_calls,b0=alloc_bytes;
        struct timespec ts0,ts1;clock_gettime(CLOCK_MONOTONIC,&ts0);
        eval(call,strlen(call),"frame.js");
        clock_gettime(CLOCK_MONOTONIC,&ts1);
        /* MEGA_TIMES: one line a frame (scene, its t, host us of frame()). */
        if(getenv("MEGA_TIMES"))printf("TIME %u %u %.1f\n",(unsigned)num("scene")&7,(unsigned)num("t"),
            (ts1.tv_sec-ts0.tv_sec)*1e6+(ts1.tv_nsec-ts0.tv_nsec)/1e3);
        if(sc<8){st_frames[sc]++;st_calls[sc]+=alloc_calls-a0;st_bytes[sc]+=alloc_bytes-b0;
            const double us=(ts1.tv_sec-ts0.tv_sec)*1e6+(ts1.tv_nsec-ts0.tv_nsec)/1e3;
            st_us[sc]+=us;if(us>st_usmax[sc])st_usmax[sc]=us;}
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
        /* MEGA_MEMDUMP_AT=t: the collected heap's breakdown after frame t. */
        if(getenv("MEGA_MEMDUMP_AT")&&strtoul(getenv("MEGA_MEMDUMP_AT"),NULL,0)==t){
            JSMemoryUsage mu;JS_ComputeMemoryUsage(rt,&mu);JS_DumpMemoryUsage(stdout,&mu,rt);
            vmtest_vmstack_report(rt,stdout);}
        if(getenv("MEGA_HEAPTRACE")&&!(t%8))
            printf("heap t=%u live=%zu ...%s\n",t,h,strlen(scene_log)>12?scene_log+strlen(scene_log)-12:scene_log);
        if(ppm_dir){
            /* NEWS; TWIST iris and twist; ZENITH (HUD, barrel roll); LIMIT; the
             * zoom into the Act II monitor set (796..); MID tier (936..). */
            static const unsigned picks[]={10,52,100,150,190,220,250,290,330,360,
                                           820,850,880,960,1000,1040};
            /* MEGA_PICKS="t,t,..." overrides the frames written. */
            const char *pk=getenv("MEGA_PICKS");
            if(pk){for(const char *c=pk;*c;){char *e;unsigned long v=strtoul(c,&e,10);
                    if(e==c)break;
                    if(v==t)ppm(ppm_dir,t);
                    c=*e?e+1:e;}}
            else for(unsigned i=0;i<sizeof picks/sizeof picks[0];i++)if(picks[i]==t)ppm(ppm_dir,t);
        }
    }
    if(getenv("MEGA_MEMDUMP_END")){JS_RunGC(rt);JSMemoryUsage m;JS_ComputeMemoryUsage(rt,&m);JS_DumpMemoryUsage(stdout,&m,rt);}
    if(!baseline){
        unsigned peak=(unsigned)num("__mega.peak"),total=(unsigned)num("__mega.total");
        unsigned maxreg=max_frame_reg;
        printf("plans: peak live %u (limit 32), registered %u over %u frames (%.1f loops), "
               "unregistered %u, most registrations in one frame %u, frames with two sets live %u\n",
               peak,total,frames,(double)frames/loop,(unsigned)num("__mega.unreg"),maxreg,coexist_frames);
        printf("host time: max register() per frame %.3f ms, max draw() per frame %.3f ms "
               "(host CPU, not device time)\n",reg_ns_max/1e6,draw_ns_max/1e6);
        printf("scenes: %s\n",scene_log);
        if(getenv("MEGA_SCENESTATS")){
            static const char *nm[8]={"CALL","ROUTE","VOYAGE","TWIST","ZENITH","CORE","LOST","WAKE"};
            unsigned long long tf=0,tc=0,tb=0;double tu=0;
            for(unsigned i=0;i<8;i++){if(!st_frames[i])continue;
                printf("scene %-6s frames %4llu allocs/frame %7.1f bytes/frame %8.0f host us/frame %7.1f max %7.1f\n",nm[i],
                       st_frames[i],(double)st_calls[i]/st_frames[i],(double)st_bytes[i]/st_frames[i],st_us[i]/st_frames[i],st_usmax[i]);
                tf+=st_frames[i];tc+=st_calls[i];tb+=st_bytes[i];tu+=st_us[i];}
            if(tf)printf("scene ALL    frames %4llu allocs/frame %7.1f bytes/frame %8.0f host us/frame %7.1f\n",tf,(double)tc/tf,(double)tb/tf,tu/tf);
        }
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
    printf("panel: %llu bytes sent over %u frames (%.0f a frame; a whole frame is 64800)\n",
           panel_bytes,frames,frames?(double)panel_bytes/frames:0.0);
    ok=ok&&!exceptions&&!present_failures;
    pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
    printf("%s\n",ok?"MEGADEMO_HOST PASS":"MEGADEMO_HOST FAIL");
    return ok?0:1;
}
