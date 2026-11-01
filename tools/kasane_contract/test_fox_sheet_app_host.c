/* Real neutral app/chunks, QuickJS and native presentation. Host only. */
#define main inherited_megademo_main
#include "test_megademo_app_host.c"
#undef main

/* Match the same per-live-block QuickJS accounting as the FOX fixture,
 * without enabling the unrelated whole-app native-allocation observer. */
#if defined(__APPLE__)
#define SHEET_OVERHEAD 0u
#else
#define SHEET_OVERHEAD 8u
#endif
static size_t sheet_live_blocks,sheet_accounted_peak;
static void sheet_sample(void){
    size_t used=cur_bytes+SHEET_OVERHEAD*sheet_live_blocks;
    if(used>sheet_accounted_peak)sheet_accounted_peak=used;
}
static void *sheet_malloc(void *o,size_t n){void *p=pk_malloc(o,n);if(p)++sheet_live_blocks;sheet_sample();return p;}
static void *sheet_calloc(void *o,size_t n,size_t z){void *p=pk_calloc(o,n,z);if(p)++sheet_live_blocks;sheet_sample();return p;}
static void sheet_free(void *o,void *p){if(p)--sheet_live_blocks;pk_free(o,p);sheet_sample();}
static void *sheet_realloc(void *o,void *p,size_t n){
    bool had=p!=NULL;void *next=pk_realloc(o,p,n);
    if(!had&&next)++sheet_live_blocks;
    if(had&&!n)--sheet_live_blocks;
    sheet_sample();return next;
}
static const JSMallocFunctions SHEET_MF={sheet_calloc,sheet_malloc,sheet_free,sheet_realloc,pk_usable};

static unsigned sheet_frames;
static bool sheet_accounting=true;
static void sheet_phase(const char *name){
    size_t used=0,limit=0;JS_GetMemoryCounters(rt,&used,&limit);
    sheet_accounting=sheet_accounting&&used==cur_bytes+SHEET_OVERHEAD*sheet_live_blocks;
    printf("SHEET_PHASE name=%s rounded=%zu peak=%zu max_request=%zu accounted=%zu cap=%zu live=%zu\n",
        name,cur_bytes,peak_bytes,largest_guest_request,used,limit,sheet_live_blocks);
}
static bool sheet_script(const char *s){return eval(s,strlen(s),"fox-sheet-contract.js");}
static bool sheet_reference(void){
    static uint16_t saved[240*135];memcpy(saved,panel,sizeof saved);
    pocket_kasane_invalidate();present((uint64_t)sheet_frames*33333u);
    return !present_failures&&!memcmp(saved,panel,sizeof saved);
}
static bool sheet_present(void){
    present((uint64_t)sheet_frames*33333u);
    bool ok=!present_failures&&sheet_reference();
    const char *folder=getenv("FOX_SHEET_CAPTURE_DIR");if(folder&&sheet_frames<9)ppm(folder,sheet_frames);
    ++sheet_frames;return ok;
}
static bool sheet_draw(double yaw,double light){
    char script[128];snprintf(script,sizeof script,"FoxSheetApp.select(%.9g,%.9g);frame(0)",yaw,light);
    return sheet_script(script)&&sheet_present();
}
static bool sheet_button(unsigned buttons){
    char script[48];snprintf(script,sizeof script,"frame(%u)",buttons);
    return sheet_script(script)&&sheet_present();
}
int main(void){
    rt=JS_NewRuntime2(&SHEET_MF,NULL);if(!rt)return 2;ctx=JS_NewContext(rt);if(!ctx)return 2;
    host_capabilities_clear();
    if(sizeof(void*)==4){JS_SetMemoryLimit(rt,163840);JS_SetGCThreshold(rt,81920);}
    sheet_phase("runtime");
    bool ok=pocket_kasane_install(ctx,NULL)==ESP_OK&&pocket_shape_install(ctx,NULL)==ESP_OK;
    sheet_phase("api");JSValue global=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,global,"__log",JS_NewCFunction(ctx,js_log,"log",1));
    JS_SetPropertyStr(ctx,global,"__now",JS_NewCFunction(ctx,js_now,"now",0));
    JS_SetPropertyStr(ctx,global,"__load",JS_NewCFunction(ctx,js_load,"load",1));JS_FreeValue(ctx,global);
    setenv("MEGA_CHUNKS","apps/foxsheet/chunks.txt",1);read_chunks();
    ok=ok&&sheet_script(PRELUDE);
    FILE *file=fopen("apps/foxsheet/fox_sheet.js","rb");if(!file)return 2;
    char source[4096];size_t bytes=fread(source,1,sizeof source-1,file);bool complete=feof(file);fclose(file);source[bytes]=0;
    ok=ok&&complete&&eval(source,bytes,"apps/foxsheet/fox_sheet.js");sheet_phase("entry-retired");
    for(int light=-150;ok&&light<=150;light+=150)
        for(int yaw=-35;ok&&yaw<=35;yaw+=35)ok=sheet_draw(yaw,light);
    sheet_phase("matrix9");
    for(unsigned k=0;ok&&k<=30;k++)ok=sheet_draw(-35.+70.*k/30.,0);
    for(unsigned k=0;ok&&k<=60;k++)ok=sheet_draw(0,-180.+6.*k);
    static uint16_t stopped[240*135];
    if(ok){ok=sheet_draw(0,0);memcpy(stopped,panel,sizeof stopped);}
    for(unsigned k=0;ok&&k<8;k++)ok=sheet_draw(0,0)&&!memcmp(stopped,panel,sizeof stopped);
    sheet_phase("steady");
    ok=ok&&sheet_script("(()=>{const before={},after={};FoxSheetApp.read(before);"
        "for(const args of [[36,0],[0,181],[NaN,0],[0,Infinity],['0',0],[0,true]]){let caught=false;"
        "try{FoxSheetApp.select(...args)}catch(e){caught=true}if(!caught)throw Error('invalid select accepted');"
        "FoxSheetApp.read(after);for(const key of ['loaded','yaw','lightYaw','tick'])if(before[key]!==after[key])throw Error('partial select');}})()");
    ok=ok&&sheet_button(0x80)&&sheet_script("(()=>{const s=FoxSheetApp.read({});if(s.yaw!==-35)throw Error('left')})()")&&sheet_button(0);
    ok=ok&&sheet_button(0x20)&&sheet_script("(()=>{const s=FoxSheetApp.read({});if(s.yaw!==0)throw Error('right')})()")&&sheet_button(0);
    ok=ok&&sheet_button(0x40)&&sheet_script("(()=>{const s=FoxSheetApp.read({});if(s.lightYaw!==60)throw Error('light')})()")&&sheet_button(0);
    ok=ok&&sheet_button(0x4000)&&sheet_button(0)&&sheet_script("(()=>{const s=FoxSheetApp.read({});if(!s.loaded||s.yaw||s.lightYaw)throw Error('reset')})()");
    if(ok)memcpy(stopped,panel,sizeof stopped);
    ok=ok&&sheet_button(0x10)&&sheet_button(0x10)&&sheet_button(0);
    ok=ok&&sheet_script("if(FoxSheetApp.read({}).loaded)throw Error('held UP toggled twice')");
    /* Released UI changes its label; the submitted image area is retained. */
    ok=ok&&!memcmp(stopped+16*240,panel+16*240,105*240*sizeof *panel);
    ok=ok&&sheet_button(0x10)&&sheet_button(0)&&!memcmp(stopped,panel,sizeof stopped);
    /* A synthetic native-open failure is an app recovery test, not an OOM
     * measurement. The core's allocator-failure suite tests actual OOM sites. */
    ok=ok&&sheet_script("(()=>{FoxSheetApp.release();const backend=pocket.ayatsuri,open=backend.modelBuiltin;let rejected=false;"
        "backend.modelBuiltin=()=>{throw Error('injected model load')};try{FoxSheetApp.select(0,0);frame(0)}catch(e){rejected=true}"
        "finally{backend.modelBuiltin=open}if(!rejected)throw Error('load failure injection missed')})()");
    ok=ok&&sheet_reference()&&!memcmp(stopped,panel,sizeof stopped);
    ok=ok&&sheet_draw(0,0)&&!memcmp(stopped,panel,sizeof stopped);sheet_phase("failure-recovered");
    for(unsigned cycle=0;ok&&cycle<6;cycle++){
        memcpy(stopped,panel,sizeof stopped);
        ok=sheet_script("FoxSheetApp.release();FoxSheetApp.release();if(FoxSheetApp.read({}).loaded)throw Error('release state')");
        if(ok)sheet_phase("released");
        ok=ok&&sheet_reference()&&!memcmp(stopped,panel,sizeof stopped);
        ok=ok&&sheet_draw(0,0)&&!memcmp(stopped,panel,sizeof stopped);
        if(ok)sheet_phase("recreated");
    }
    JSOOMCanary canary;JS_TakeOOMCanary(rt,&canary);size_t used=0,cap=0;JS_GetMemoryCounters(rt,&used,&cap);
    printf("SHEET_HOST frames=%u pointer_bits=%zu peak_requested=%zu accounted_peak=%zu max_request=%zu cap=%zu eager_gc=0 full_reference=1 quota=%u allocator=%u formula=%u\n",
        sheet_frames,sizeof(void*)*8,peak_bytes,sheet_accounted_peak,largest_guest_request,cap,canary.quota_count,canary.allocator_count,(unsigned)sheet_accounting);
    ok=ok&&sheet_frames==130&&!exceptions&&!present_failures&&!canary.quota_count&&!canary.allocator_count&&sheet_accounting;
    ok=ok&&(sizeof(void*)!=4||sheet_accounted_peak<=163840);
    pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);ctx=NULL;rt=NULL;
    ok=ok&&!cur_bytes&&!sheet_live_blocks;
    printf("SHEET_HOST_%s released_guest=%zu live_blocks=%zu\n",ok?"PASS":"FAIL",cur_bytes,sheet_live_blocks);return ok?0:1;
}
