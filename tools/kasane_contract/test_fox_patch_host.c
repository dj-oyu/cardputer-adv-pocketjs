/* HOST ONLY: existing project/shade/fill and native Model painter passes.
 * Timing separates the JS turn from native presentation; neither is device FPS. */
#define main inherited_megademo_main
#include "test_megademo_app_host.c"
#undef main
#include "pocket_ayatsuri_assets.h"
#include "ayatsuri/ayatsuri_model.h"

#if defined(__APPLE__)
#define PATCH_OVERHEAD 0u
#else
#define PATCH_OVERHEAD 8u
#endif
static size_t patch_blocks,patch_accounted_peak;
static bool patch_accounting_valid=true;
static void patch_sample(void){size_t n=cur_bytes+PATCH_OVERHEAD*patch_blocks;if(n>patch_accounted_peak)patch_accounted_peak=n;}
static void *patch_malloc(void *o,size_t n){void *p=pk_malloc(o,n);if(p)++patch_blocks;patch_sample();return p;}
static void *patch_calloc(void *o,size_t n,size_t s){void *p=pk_calloc(o,n,s);if(p)++patch_blocks;patch_sample();return p;}
static void patch_free(void *o,void *p){if(p)--patch_blocks;pk_free(o,p);patch_sample();}
static void *patch_realloc(void *o,void *p,size_t n){bool had=p!=NULL;void *next=pk_realloc(o,p,n);if(!had&&next)++patch_blocks;if(had&&!n)--patch_blocks;patch_sample();return next;}
static const JSMallocFunctions PATCH_MF={patch_calloc,patch_malloc,patch_free,patch_realloc,pk_usable};
static JSValue study,draw_fn,stats_fn,release_fn,stats_out;
static unsigned patch_frames;
static double js_us[128],present_us[128];
static unsigned long long turn_calls,turn_bytes;
static double patch_now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (double)t.tv_sec*1e6+(double)t.tv_nsec/1e3;}
static bool patch_exception(JSValue value,const char *where){
    if(!JS_IsException(value))return false;
    JSValue error=JS_GetException(ctx);const char *s=JS_ToCString(ctx,error);
    fprintf(stderr,"FOX_PATCH_EXCEPTION phase=%s first=%s\n",where,s?s:"exception");if(s)JS_FreeCString(ctx,s);
    JSValue stack=JS_GetPropertyStr(ctx,error,"stack");s=JS_ToCString(ctx,stack);if(s){fprintf(stderr,"%s\n",s);JS_FreeCString(ctx,s);}
    JS_FreeValue(ctx,stack);JS_FreeValue(ctx,error);++exceptions;return true;
}
static bool patch_end_turn(void){
    JSContext *job;int status;while((status=JS_ExecutePendingJob(rt,&job))>0){}
    if(status<0){++exceptions;return false;}JS_VMStackTrim(rt);pocket_kasane_end_turn();return true;
}
static bool patch_load(const char *path){
    FILE *f=fopen(path,"rb");if(!f){fprintf(stderr,"FOX_PATCH_INPUT missing=%s\n",path);return false;}
    if(fseek(f,0,SEEK_END)){fclose(f);return false;}long size=ftell(f);rewind(f);
    if(size<=0||size>(1<<20)){fclose(f);return false;}char *source=malloc((size_t)size+1);if(!source){fclose(f);return false;}
    size_t n=fread(source,1,(size_t)size,f);fclose(f);source[n]=0;
    bool ok=n==(size_t)size&&eval(source,n,path);free(source);return ok;
}
static bool patch_reference(uint64_t time){static uint16_t saved[240*135];memcpy(saved,panel,sizeof saved);pocket_kasane_invalidate();present(time);return !present_failures&&!memcmp(saved,panel,sizeof saved);}
static uint32_t patch_hash(void){uint32_t h=2166136261u;for(unsigned i=0;i<240*135;i++){h=(h^(panel[i]&255u))*16777619u;h=(h^(panel[i]>>8))*16777619u;}return h;}
static bool patch_draw(double yaw,double light,bool capture){
    if(patch_frames>=128)return false;
    JSValue args[2]={JS_NewFloat64(ctx,yaw),JS_NewFloat64(ctx,light)};
    const unsigned long long before_calls=alloc_calls,before_bytes=alloc_bytes;double start=patch_now();
    JSValue value=JS_Call(ctx,draw_fn,study,2,args);bool ok=!patch_exception(value,"draw");JS_FreeValue(ctx,value);
    ok=patch_end_turn()&&ok;js_us[patch_frames]=patch_now()-start;turn_calls+=alloc_calls-before_calls;turn_bytes+=alloc_bytes-before_bytes;
    JS_FreeValue(ctx,args[0]);JS_FreeValue(ctx,args[1]);if(!ok)return false;
    const uint64_t time=(uint64_t)patch_frames*33333u;start=patch_now();present(time);present_us[patch_frames]=patch_now()-start;
    if(present_failures||!patch_reference(time))return false;
    printf("FOX_PATCH_FRAME index=%u yaw=%.9g light=%.9g hash=%08x js_us=%.3f present_us=%.3f\n",patch_frames,yaw,light,patch_hash(),js_us[patch_frames],present_us[patch_frames]);
    const char *folder=getenv("FOX_PATCH_CAPTURE_DIR");if(capture&&folder)ppm(folder,patch_frames);
    ++patch_frames;return true;
}
static double patch_stat(const char *name){JSValue v=JS_GetPropertyStr(ctx,stats_out,name);double n=-1;if(JS_ToFloat64(ctx,&n,v)<0)n=-1;JS_FreeValue(ctx,v);return n;}
static bool patch_phase(const char *name,bool with_stats){
    unsigned long long before=alloc_calls;
    if(with_stats){JSValue v=JS_Call(ctx,stats_fn,study,1,&stats_out);bool bad=patch_exception(v,"stats");JS_FreeValue(ctx,v);if(bad)return false;}
    const unsigned long long stats_calls=alloc_calls-before;size_t used=0,limit=0;JS_GetMemoryCounters(rt,&used,&limit);
    patch_accounting_valid=patch_accounting_valid&&used==cur_bytes+PATCH_OVERHEAD*patch_blocks;
    printf("FOX_PATCH_PHASE name=%s requested_current=%zu requested_peak=%zu max_request=%zu guest_current=%zu guest_peak=%zu cap=%zu stats_alloc_calls=%llu",name,cur_bytes,peak_bytes,largest_guest_request,used,patch_accounted_peak,limit,stats_calls);
    if(with_stats)printf(" vertices=%.0f triangles=%.0f fill_calls=%.0f culled=%.0f native_passes=%.0f typed_bytes=%.0f",patch_stat("vertices"),patch_stat("triangles"),patch_stat("fillCalls"),patch_stat("culled"),patch_stat("nativePasses"),patch_stat("typedBytes"));
    putchar('\n');return patch_accounting_valid&&(!with_stats||
        (patch_stat("vertices")>0&&patch_stat("triangles")>0&&patch_stat("typedBytes")>0&&
         patch_stat("nativePasses")==1&&patch_stat("fillCalls")>=0&&patch_stat("fillCalls")<=2*patch_stat("triangles")&&
         patch_stat("culled")>=0&&patch_stat("culled")<=patch_stat("triangles")));
}
static bool patch_invalid(double yaw,double light){
    static uint16_t old[240*135];memcpy(old,panel,sizeof old);JSValue args[2]={JS_NewFloat64(ctx,yaw),JS_NewFloat64(ctx,light)};
    JSValue v=JS_Call(ctx,draw_fn,study,2,args);const bool rejected=JS_IsException(v);
    if(rejected){JSValue error=JS_GetException(ctx);JS_FreeValue(ctx,error);}JS_FreeValue(ctx,v);JS_FreeValue(ctx,args[0]);JS_FreeValue(ctx,args[1]);
    return patch_end_turn()&&rejected&&patch_reference((uint64_t)patch_frames*33333u)&&!memcmp(old,panel,sizeof old);
}
static int compare_double(const void *a,const void *b){double x=*(const double *)a,y=*(const double *)b;return (x>y)-(x<y);}
static void patch_timing(const char *name,const double *values,unsigned start){
    double sorted[128];unsigned count=patch_frames-start;memcpy(sorted,values+start,count*sizeof *sorted);qsort(sorted,count,sizeof *sorted,compare_double);
    double median=count%2?sorted[count/2]:(sorted[count/2-1]+sorted[count/2])*.5;
    printf("FOX_PATCH_TIME scope=%s samples=%u median_us=%.3f max_us=%.3f\n",name,count,median,sorted[count-1]);
}
int main(void){
    rt=JS_NewRuntime2(&PATCH_MF,NULL);if(!rt)return 2;ctx=JS_NewContext(rt);if(!ctx){JS_FreeRuntime(rt);return 2;}host_capabilities_clear();
    if(sizeof(void*)==4){JS_SetMemoryLimit(rt,163840);JS_SetGCThreshold(rt,81920);}
    bool ok=patch_phase("runtime",false);pocket_kasane_install(ctx,NULL);pocket_shape_install(ctx,NULL);ok=ok&&patch_phase("api",false);
    const char *prelude="globalThis.pocket={kasane:globalThis.kasane,hone:globalThis.hone,ayatsuri:globalThis.ayatsuri};";
    ok=ok&&eval(prelude,strlen(prelude),"fox-patch-prelude.js");
    const char *variant=getenv("FOX_PATCH_VARIANT");
    if(variant){JSValue g=JS_GetGlobalObject(ctx);JS_SetPropertyStr(ctx,g,"__foxPatchVariant",JS_NewString(ctx,variant));JS_FreeValue(ctx,g);}
    ok=ok&&patch_load("apps/lib/ayatsuri_model.js")&&patch_load("apps/lib/ayatsuri_clip.js")&&patch_load("tools/character/fox_patch_fixture.js");
    JSValue global=JS_GetGlobalObject(ctx);study=JS_GetPropertyStr(ctx,global,"FoxPatchStudy");JS_FreeValue(ctx,global);
    draw_fn=JS_GetPropertyStr(ctx,study,"draw");stats_fn=JS_GetPropertyStr(ctx,study,"stats");release_fn=JS_GetPropertyStr(ctx,study,"release");stats_out=JS_NewObject(ctx);
    ok=ok&&JS_IsFunction(ctx,draw_fn)&&JS_IsFunction(ctx,stats_fn)&&JS_IsFunction(ctx,release_fn)&&patch_phase("fixture",false);
    const uint8_t *cold=NULL;size_t cold_bytes=0;ayatsuri_model model;
    if(pocket_ayatsuri_builtin_asset("fox-face",&cold,&cold_bytes)&&ayatsuri_model_open(cold,cold_bytes,&model)==AYATSURI_MODEL_OK)
        printf("FOX_PATCH_NATIVE cold_bytes=%zu nodes=%u arena_payload_bytes=%zu guest_charged=1 native_heap_unmeasured=1\n",cold_bytes,model.node_count,ayatsuri_model_instance_bytes(&model));else ok=false;
    for(int yaw=-35;ok&&yaw<=35;yaw+=35)ok=patch_draw(yaw,0,true);
    if(ok)ok=patch_phase("captures",true);
    for(unsigned i=0;ok&&i<=60;i++)ok=patch_draw(-35.+70.*i/60.,0,false);
    uint32_t light_hash[2]={0,0};unsigned light_index=0;
    for(int light=-90;ok&&light<=90;light+=180){ok=patch_draw(0,light,false);light_hash[light_index++]=patch_hash();}
    if(ok)ok=light_hash[0]!=light_hash[1];
    if(ok)ok=patch_draw(0,0,false);
    static uint16_t stopped[240*135];memcpy(stopped,panel,sizeof stopped);
    for(unsigned i=0;ok&&i<8;i++)ok=patch_draw(0,0,false)&&!memcmp(stopped,panel,sizeof stopped);
    if(ok)ok=patch_phase("steady",true)&&patch_invalid(36,0)&&patch_invalid(NAN,0)&&patch_invalid(0,181)&&patch_invalid(0,INFINITY);
    for(unsigned i=0;ok&&i<3;i++){
        JSValue v=JS_Call(ctx,release_fn,study,0,NULL);ok=!patch_exception(v,"release");JS_FreeValue(ctx,v);ok=patch_end_turn()&&ok;
        if(ok)ok=patch_phase("released",false)&&patch_reference((uint64_t)patch_frames*33333u)&&!memcmp(stopped,panel,sizeof stopped);
        if(ok)ok=patch_draw(0,0,false)&&!memcmp(stopped,panel,sizeof stopped)&&patch_phase("recreated",true);
    }
    if(patch_frames){patch_timing("js_turn_all",js_us,0);patch_timing("present_all",present_us,0);}
    if(patch_frames>3){patch_timing("js_turn_after_captures",js_us,3);patch_timing("present_after_captures",present_us,3);}
    JSOOMCanary oom;JS_TakeOOMCanary(rt,&oom);
    printf("FOX_PATCH_HOST frames=%u pointer_bits=%zu requested_peak=%zu max_request=%zu guest_peak=%zu cap=%u quota=%u allocator=%u accounting=%u eager_gc=0 full_reference=1 turn_alloc_calls=%llu turn_alloc_bytes=%llu\n",patch_frames,sizeof(void*)*8,peak_bytes,largest_guest_request,patch_accounted_peak,sizeof(void*)==4?163840u:0u,oom.quota_count,oom.allocator_count,(unsigned)patch_accounting_valid,turn_calls,turn_bytes);
    ok=ok&&patch_frames==78&&!exceptions&&!present_failures&&!oom.quota_count&&!oom.allocator_count&&patch_accounting_valid;
    if(JS_IsFunction(ctx,release_fn)){JSValue v=JS_Call(ctx,release_fn,study,0,NULL);if(patch_exception(v,"final-release"))ok=false;JS_FreeValue(ctx,v);}
    JS_FreeValue(ctx,stats_out);JS_FreeValue(ctx,release_fn);JS_FreeValue(ctx,stats_fn);JS_FreeValue(ctx,draw_fn);JS_FreeValue(ctx,study);
    pocket_kasane_reset();host_capabilities_clear();JS_FreeContext(ctx);JS_FreeRuntime(rt);ctx=NULL;rt=NULL;ok=ok&&!cur_bytes&&!patch_blocks;
    printf("FOX_PATCH_%s released_guest=%zu live_blocks=%zu\n",ok?"PASS":"FAIL",cur_bytes,patch_blocks);return ok?0:1;
}
