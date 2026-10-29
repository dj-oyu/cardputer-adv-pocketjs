/* Reuse the real Kasane presentation/allocator fixture, without its
 * MEGADEMO-specific scene and plan assertions. No device or serial I/O. */
#define main fixture_megademo_main
#include "test_megademo_app_host.c"
#undef main

static unsigned failure_strip;
static ksn_result fail_second_strip(void *o,uint16_t y,uint16_t rows,const uint16_t *pixels){
    if(++failure_strip==2)return KSN_IO;
    return send_strip(o,y,rows,pixels);
}
static bool full_reference(uint64_t now_us){
    static uint16_t incremental[240*135];
    memcpy(incremental,panel,sizeof incremental);
    pocket_kasane_invalidate();present(now_us);
    return memcmp(incremental,panel,sizeof incremental)==0;
}

static bool primitive_contract(void){
    const char *script=
      "globalThis.__ellipsoidTest=(()=>{"
      "const H=pocket.kasane.procedural,S=pocket.shapes;"
      "const screen=new Float32Array([10,0,0,120,0,10,0,67]);"
      "const normal=new Float32Array([1,0,0,0,1,0,0,0,1]);"
      "const light=new Float32Array([0,0,1,0,0,1,1,1,1]);"
      "const material=new Float32Array([1,0,0,1,0,1,1,1]);"
      "const surface=new Uint16Array([0xf800,0,255,128,0]);"
      "const texture=new Float32Array([1,0,0,0,1,0,0,0,1]);"
      "const draw=()=>S.ellipsoid(screen,normal,light,material,surface,texture);"
      "const mustThrow=(f,code)=>{try{f()}catch(e){if(e.code===code)return;throw e}throw Error('missing '+code)};"
      "const resource=H.resource({maxSegments:1024});"
      "pocket.kasane.replace(tx=>{tx.background(0x000000ff);tx.image({resource,bounds:[0,0,240,135]})});"
      "H.beginFrame(0);draw();surface[0]=0x001f;H.commit();"
      "return {check:()=>{"
      "H.beginFrame(0);screen[0]=NaN;mustThrow(draw,'INVALID_ARGUMENT');screen[0]=10;"
      "normal[8]=0;mustThrow(draw,'INVALID_ARGUMENT');normal[8]=1;"
      "surface[4]=4;mustThrow(draw,'INVALID_ARGUMENT');surface[4]=0;"
      "mustThrow(()=>S.ellipsoid(screen,normal,light,material,surface,new Float32Array(8)),'INVALID_ARGUMENT');"
      "draw();"
      "screen[0]=120;screen[5]=67;draw();draw();mustThrow(draw,'LIMIT_EXCEEDED');"
      "mustThrow(()=>H.commit(),'BUSY');H.beginFrame(0);screen[0]=10;screen[5]=10;draw();H.commit();"
      "},light:(front)=>{material[0]=.25;material[1]=.75;light[0]=front?0:-1;light[2]=front?1:0;H.beginFrame(0);draw();H.commit()},"
      "uv:(offset)=>{surface[1]=0xf800;surface[4]=2;texture[5]=offset;texture[7]=0;texture[8]=.4;H.beginFrame(0);draw();H.commit()}"
      "}})();";
    if(!eval(script,strlen(script),"ellipsoid-contract.js"))return false;
    present(0);
    if(panel[67*240+120]!=0xf800){printf("immutable ellipsoid packet changed after input mutation\n");return false;}
    const char *check="__ellipsoidTest.check();";
    if(!eval(check,strlen(check),"ellipsoid-contract.js"))return false;
    present(1);
    if(panel[67*240+120]!=0x001f)return false;
    const char *side="__ellipsoidTest.light(false);",*front="__ellipsoidTest.light(true);";
    if(!eval(side,strlen(side),"ellipsoid-light.js"))return false;
    present(2);
    const uint16_t side_color=panel[67*240+120];
    if(!eval(front,strlen(front),"ellipsoid-light.js"))return false;
    present(3);
    if(side_color==panel[67*240+120])return false;
    const char *uv0="__ellipsoidTest.uv(0);",*uv1="__ellipsoidTest.uv(-.3);";
    if(!eval(uv0,strlen(uv0),"ellipsoid-uv.js"))return false;
    present(4);
    const uint16_t uv_color=panel[67*240+120];
    if(!eval(uv1,strlen(uv1),"ellipsoid-uv.js"))return false;
    present(5);
    if(uv_color==panel[67*240+120])return false;
    if(!eval(front,strlen(front),"ellipsoid-repair.js"))return false;
    pocket_kasane_invalidate();
    ksn_display_port failing={.strip=get_strip,.present=fail_second_strip,
        .width=240,.height=135,.strip_rows=8,.text=&ksn_font_port};
    ksn_render_stats stats;
    if(pocket_kasane_present(&failing,&stats)!=KSN_IO)return false;
    const char *busy="(()=>{try{pocket.kasane.procedural.beginFrame(0)}catch(e){if(e.code==='BUSY')return;throw e}throw Error('missing repair BUSY')})()";
    if(!eval(busy,strlen(busy),"ellipsoid-repair.js"))return false;
    present(6);
    if(!full_reference(6))return false;
    const char *cleanup="delete globalThis.__ellipsoidTest;";
    return eval(cleanup,strlen(cleanup),"ellipsoid-contract.js");
}

int main(int argc,char **argv){
    const char *path="apps/character/character_lab.js";
    FILE *f=fopen(path,"rb");if(!f)return 2;
    char src[4096];size_t n=fread(src,1,sizeof(src)-1,f);
    if(ferror(f)||!feof(f)){fclose(f);return 2;}fclose(f);src[n]=0;
    rt=JS_NewRuntime2(&PEAK_MF,NULL);ctx=JS_NewContext(rt);host_capabilities_clear();
    const char *limit=getenv("CHARACTER_HEAP_LIMIT");
    if(limit){size_t cap=strtoul(limit,NULL,0);JS_SetMemoryLimit(rt,cap);JS_SetGCThreshold(rt,cap/2);}
    pocket_kasane_install(ctx,NULL);pocket_shape_install(ctx,NULL);
    JSValue g=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,g,"__log",JS_NewCFunction(ctx,js_log,"log",1));
    JS_SetPropertyStr(ctx,g,"__now",JS_NewCFunction(ctx,js_now,"now",0));
    JS_SetPropertyStr(ctx,g,"__load",JS_NewCFunction(ctx,js_load,"load",1));JS_FreeValue(ctx,g);
    if(setenv("MEGA_CHUNKS","apps/character/chunks.txt",1))return 2;
    read_chunks();
    bool ok=eval(PRELUDE,strlen(PRELUDE),"prelude.js")&&primitive_contract();
    JS_RunGC(rt);peak_bytes=cur_bytes;largest_guest_request=0;
    ok=ok&&eval(src,n,path);
    size_t compile_peak=peak_bytes,load_request=largest_guest_request,live_max=0;unsigned frames=0;
    largest_guest_request=0;
    const unsigned per_mode=argc>1?(unsigned)strtoul(argv[1],NULL,0):301;
    const bool eager_gc=getenv("CHARACTER_NO_EAGER_GC")==NULL;
    const bool compare_full=getenv("CHARACTER_FULL_REFERENCE")!=NULL;
    printf("CHARACTER_NATIVE eager_gc=%u full_reference=%u repair_injection=PASS\n",eager_gc,compare_full);
    if(per_mode<2)return 2;
    static uint16_t stopped[240*135];
    for(unsigned actor=0;ok&&actor<3;actor++)for(unsigned mode=0;ok&&mode<6;mode++){
        for(unsigned t=0;ok&&t<per_mode;t++){
            unsigned buttons=0;
            if(t==0&&(actor||mode))buttons=mode?0x20:0x4020;
            char call[64];snprintf(call,sizeof(call),"frame(%u)",buttons);
            ok=eval(call,strlen(call),"frame.js");present((uint64_t)frames*33333u);
            if(compare_full&&!full_reference((uint64_t)frames*33333u)){
                printf("incremental/full mismatch model=%u mode=%u frame=%u\n",actor,mode,t);ok=false;
            }
            if(actor==2&&mode==0&&t==240)memcpy(stopped,panel,sizeof stopped);
            if(actor==2&&mode==0&&t==300&&memcmp(stopped,panel,sizeof stopped)){
                printf("cloud changed after stopping with breathing off\n");ok=false;
            }
            const char *ppm_dir=getenv("MEGA_PPM");
            if(ppm_dir&&(t==0||t==60||t==90||t==171||t==240||t==300))ppm(ppm_dir,frames);
            frames++;
            if(eager_gc)JS_RunGC(rt);
            size_t live=heap_used();if(live>live_max)live_max=live;
        }
        printf("CHARACTER_NATIVE model=%u mode=%u frames=%u exceptions=%u present_failures=%u live=%zu\n",
               actor,mode,frames,exceptions,present_failures,heap_used());
    }
    printf("CHARACTER_NATIVE compile_peak=%zu live_max=%zu allocator_peak=%zu load_max_request=%zu run_max_request=%zu frames=%u host_pointer_bits=%zu\n",
           compile_peak,live_max,peak_bytes,load_request,largest_guest_request,frames,sizeof(void*)*8);
    if(ok&&sizeof(void*)==4&&limit){
        /* The smaller cloud is resident. Force human construction to fail,
         * retaining the native picture; then restore the ordinary guest cap. */
        memcpy(stopped,panel,sizeof stopped);
        const size_t before_switch=heap_used();
        JS_SetMemoryLimit(rt,151000);
        const char *attempt="(()=>{try{frame(16384)}catch(e){return true}return false})()";
        JSValue result=JS_Eval(ctx,attempt,strlen(attempt),"switch-oom.js",JS_EVAL_TYPE_GLOBAL);
        JS_SetMemoryLimit(rt,strtoul(limit,NULL,0));
        ok=!JS_IsException(result)&&JS_ToBool(ctx,result)==1;
        JS_FreeValue(ctx,result);JS_RunGC(rt);present((uint64_t)frames*33333u);
        ok=ok&&memcmp(stopped,panel,sizeof stopped)==0;
        const size_t failed_switch=heap_used();
        ok=ok&&eval("frame(0)",8,"switch-retry.js");present((uint64_t)(frames+1)*33333u);
        JS_RunGC(rt);
        printf("CHARACTER_NATIVE switch_failure_recovery=%s before=%zu failed=%zu recovered=%zu\n",
               ok?"PASS":"FAIL",before_switch,failed_switch,heap_used());
    }
    ok=ok&&!exceptions&&!present_failures;
    pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
    if(cur_bytes){printf("guest allocator retained %zu bytes after realm release\n",cur_bytes);ok=false;}
    printf("CHARACTER_NATIVE released_guest=%zu\n",cur_bytes);
    printf("%s\n",ok?"CHARACTER_NATIVE_PASS":"CHARACTER_NATIVE_FAIL");return ok?0:1;
}
