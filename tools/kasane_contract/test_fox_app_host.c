/* Real app/chunks and native presentation; no device or wall-clock waits. */
#define main inherited_megademo_main
#define js_load fox_base_load
#include "test_megademo_app_host.c"
#undef main
#undef js_load

static JSValue js_load(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    const char *name=argc?JS_ToCString(c,argv[0]):NULL;
    JSValue value=fox_base_load(c,self,argc,argv);size_t used=0,limit=0;
    JS_GetMemoryCounters(JS_GetRuntime(c),&used,&limit);
    printf("FOX_LOAD name=%s used=%zu limit=%zu exception=%u\n",name?name:"?",used,limit,(unsigned)JS_IsException(value));
    if(name)JS_FreeCString(c,name);
    return value;
}

static unsigned fox_frames;
static bool full_reference(uint64_t time){
    static uint16_t saved[240*135];memcpy(saved,panel,sizeof saved);
    pocket_kasane_invalidate();present(time);return !memcmp(saved,panel,sizeof saved);
}
static bool fox_draw(unsigned expression,double yaw,double pitch,double light){
    char call[180];
    snprintf(call,sizeof call,"FoxFaceLab.select(%u,%.9g,%.9g,%.9g);frame(0)",expression,yaw,pitch,light);
    if(!eval(call,strlen(call),"fox-frame.js"))return false;
    const uint64_t time=(uint64_t)fox_frames*33333u;
    present(time);if(present_failures||!full_reference(time))return false;
    const char *folder=getenv("FOX_CAPTURE_DIR");if(folder&&fox_frames<42)ppm(folder,fox_frames);
    ++fox_frames;return true;
}
static bool fox_button(unsigned buttons){
    char call[48];snprintf(call,sizeof call,"frame(%u)",buttons);
    if(!eval(call,strlen(call),"fox-button.js"))return false;
    const uint64_t time=(uint64_t)fox_frames++*33333u;present(time);
    return !present_failures&&full_reference(time);
}
int main(void){
    rt=JS_NewRuntime2(&PEAK_MF,NULL);ctx=JS_NewContext(rt);host_capabilities_clear();
    if(sizeof(void*)==4){JS_SetMemoryLimit(rt,163840);JS_SetGCThreshold(rt,81920);}
    pocket_kasane_install(ctx,NULL);pocket_shape_install(ctx,NULL);
    JSValue g=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,g,"__log",JS_NewCFunction(ctx,js_log,"log",1));
    JS_SetPropertyStr(ctx,g,"__now",JS_NewCFunction(ctx,js_now,"now",0));
    JS_SetPropertyStr(ctx,g,"__load",JS_NewCFunction(ctx,js_load,"load",1));JS_FreeValue(ctx,g);
    setenv("MEGA_CHUNKS","apps/fox3d/chunks.txt",1);read_chunks();
    bool ok=eval(PRELUDE,strlen(PRELUDE),"prelude.js");
    FILE *file=fopen("apps/fox3d/fox_face.js","rb");if(!file)return 2;
    char source[4096];size_t bytes=fread(source,1,sizeof source-1,file);fclose(file);source[bytes]=0;
    ok=ok&&eval(source,bytes,"apps/fox3d/fox_face.js");
    for(unsigned e=0;ok&&e<4;++e)for(int yaw=-35;ok&&yaw<=35;yaw+=35)ok=fox_draw(e,yaw,0,0);
    for(unsigned e=0;ok&&e<4;++e)for(int pitch=-10;ok&&pitch<=10;pitch+=20)
        for(int yaw=-35;ok&&yaw<=35;yaw+=35)ok=fox_draw(e,yaw,pitch,0);
    for(int light=-150;ok&&light<=150;light+=300)
        for(int yaw=-35;ok&&yaw<=35;yaw+=35)ok=fox_draw(0,yaw,0,light);
    for(unsigned i=0;ok&&i<=120;++i)ok=fox_draw(i<61?1:2,-35.+70.*(i%61)/60.,0,0);
    for(unsigned i=0;ok&&i<=60;++i)ok=fox_draw(0,0,0,-60.+2.*i);
    static uint16_t stopped[240*135];
    if(ok){ok=fox_draw(0,0,0,0);memcpy(stopped,panel,sizeof stopped);}
    for(unsigned i=0;ok&&i<8;++i)ok=fox_draw(0,0,0,0)&&!memcmp(stopped,panel,sizeof stopped);
    const char *contract="(()=>{const a={},b={};FoxFaceLab.read(a);let rejected=false;"
        "try{FoxFaceLab.select(4,0,0,0)}catch(e){rejected=true}if(!rejected)throw Error('invalid view accepted');"
        "FoxFaceLab.read(b);if(a.expression!==b.expression||a.yaw!==b.yaw||a.pitch!==b.pitch||a.lightYaw!==b.lightYaw)throw Error('partial view');})();";
    if(ok)ok=eval(contract,strlen(contract),"fox-controls.js");
    // Every successful commit must retire before the next app turn begins.
    if(ok)ok=fox_button(0x10)&&fox_button(0)&&fox_button(0x4000)&&fox_button(0);
    const char *reset="(()=>{const b={};FoxFaceLab.read(b);if(b.expression||b.yaw||b.pitch||b.lightYaw)throw Error('reset');})();";
    if(ok)ok=eval(reset,strlen(reset),"fox-reset.js");
    JSOOMCanary canary;JS_TakeOOMCanary(rt,&canary);
    printf("FOX_HOST frames=%u pointer_bits=%zu peak_requested=%zu max_request=%zu cap=%u eager_gc=0 full_reference=1 quota=%u allocator=%u\n",
           fox_frames,sizeof(void*)*8,peak_bytes,largest_guest_request,sizeof(void*)==4?163840u:0u,canary.quota_count,canary.allocator_count);
    ok=ok&&!exceptions&&!present_failures&&!canary.quota_count&&!canary.allocator_count;
    pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);ctx=NULL;rt=NULL;
    ok=ok&&!cur_bytes;printf("FOX_HOST_%s released_guest=%zu\n",ok?"PASS":"FAIL",cur_bytes);
    return ok?0:1;
}
