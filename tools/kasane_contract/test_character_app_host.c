/* Reuse the real Kasane presentation/allocator fixture, without its
 * MEGADEMO-specific scene and plan assertions. No device or serial I/O. */
#define main fixture_megademo_main
#define js_log character_fixture_base_log
#define js_load character_fixture_base_load
#include "test_megademo_app_host.c"
#undef main
#undef js_log
#undef js_load
#include <float.h>
#include "pocket_api.h"

static unsigned fixture_starts,fixture_completes,fixture_passes,fixture_returns,fixture_return_complete,fixture_return_failure;
static bool fixture_saw_oom;
static JSValue js_log(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    if(getenv("CHARACTER_FIXTURE")&&argc&&JS_IsString(argv[0])){
        const char *line=JS_ToCString(c,argv[0]);
        if(line){
            if(!strncmp(line,"CLOUD_FIXTURE START seed=42 ",28))++fixture_starts;
            if(!strncmp(line,"CLOUD_FIXTURE COMPLETE ",23)){++fixture_completes;if(strstr(line,"result=PASS "))++fixture_passes;}
            if(!strncmp(line,"CLOUD_FIXTURE RETURN ",21)){++fixture_returns;if(strstr(line,"reason=complete"))++fixture_return_complete;if(strstr(line,"reason=failure"))++fixture_return_failure;}
            JS_FreeCString(c,line);
        }
    }
    return character_fixture_base_log(c,self,argc,argv);
}



/* Host-only observation, not another JS call frame. Canary reads consume only
 * diagnostic counters; they neither collect garbage nor mutate app clocks. */
static JSValue js_load(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    if(!getenv("CHARACTER_FIXTURE"))return character_fixture_base_load(c,self,argc,argv);
    char name[40]="?";
    if(argc&&JS_IsString(argv[0])){
        const char *value=JS_ToCString(c,argv[0]);
        if(value){snprintf(name,sizeof name,"%s",value);JS_FreeCString(c,value);}
    }
    size_t used_before=0,limit=0;JS_GetMemoryCounters(JS_GetRuntime(c),&used_before,&limit);
    JSMemoryUsage before;JS_ComputeMemoryUsage(JS_GetRuntime(c),&before);
    JSOOMCanary prior;JS_TakeOOMCanary(JS_GetRuntime(c),&prior);
    JSValue result=character_fixture_base_load(c,self,argc,argv);
    JSMemoryUsage m;JS_ComputeMemoryUsage(JS_GetRuntime(c),&m);
    JSOOMCanary canary;JS_TakeOOMCanary(JS_GetRuntime(c),&canary);
    fixture_saw_oom=fixture_saw_oom||prior.quota_count||prior.allocator_count||canary.quota_count||canary.allocator_count;
    fprintf(stderr,"CHARACTER_FIXTURE_LOAD name=%s exception=%u used_before=%zu used_after=%lld memory_used=%lld blocks=%lld limit=%zu pk_current=%zu pk_peak=%zu max_request=%zu prior_quota_rejections=%u prior_allocator_rejections=%u quota_rejections=%u allocator_rejections=%u first_req=%zu first_used=%zu\n",
        name,(unsigned)JS_IsException(result),used_before,(long long)m.malloc_size,
        (long long)m.memory_used_size,(long long)m.malloc_count,limit,cur_bytes,peak_bytes,
        largest_guest_request,prior.quota_count,prior.allocator_count,canary.quota_count,canary.allocator_count,canary.first_req,canary.first_used);
    fprintf(stderr,"CHARACTER_FIXTURE_LOAD_CATEGORIES name=%s",name);
#define FIXTURE_MEMORY_PAIR(field) fprintf(stderr," " #field "_before=%lld " #field "_after=%lld",(long long)before.field,(long long)m.field)
    FIXTURE_MEMORY_PAIR(js_func_size);FIXTURE_MEMORY_PAIR(js_func_code_size);FIXTURE_MEMORY_PAIR(js_func_count);
    FIXTURE_MEMORY_PAIR(obj_size);FIXTURE_MEMORY_PAIR(obj_count);FIXTURE_MEMORY_PAIR(prop_size);
    FIXTURE_MEMORY_PAIR(shape_size);FIXTURE_MEMORY_PAIR(atom_size);FIXTURE_MEMORY_PAIR(str_size);
    FIXTURE_MEMORY_PAIR(memory_used_size);FIXTURE_MEMORY_PAIR(array_count);FIXTURE_MEMORY_PAIR(binary_object_size);
#undef FIXTURE_MEMORY_PAIR
    fputs(" vm_segment_bytes=UNMEASURED\n",stderr);
    return result;
}

static unsigned long long core_alloc_start,core_loop_alloc_calls;
static JSValue js_core_alloc_checkpoint(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argv;
    if(!argc)core_alloc_start=alloc_calls;
    else{
        core_loop_alloc_calls=alloc_calls-core_alloc_start;
        if(core_loop_alloc_calls)return JS_ThrowInternalError(c,"compose loop allocated guest memory");
    }
    return JS_UNDEFINED;
}

static bool core_transform_contract(void){
    const char *script=
      "(()=>{const C=pocket.ayatsuri,p=new Float32Array([1,2,3,0,0,0,1,1,1]),o=new Float32Array(12);"
      "C.compose(p,null,o);__coreAlloc();for(let i=0;i<1000;i++)C.compose(p,null,o);__coreAlloc(1);"
      "const node=new Float32Array(21);node.set(p,12);C.composeNode(node,null);"
      "for(let i=0;i<12;i++)if(node[i]!==o[i])throw Error('packed compose parity');"
      "__coreAlloc();for(let i=0;i<1000;i++)C.composeNode(node,null);__coreAlloc(1);"
      "node[18]=0;const saved=new Float32Array(node);let refused=false;try{C.composeNode(node,null)}catch(e){refused=true}"
      "if(!refused)throw Error('packed compose invalid missing');for(let i=0;i<21;i++)if(node[i]!==saved[i])throw Error('packed compose partial write');"
      "for(let i=0;i<12;i++)o[i]=37;p[6]=0;let caught=false;"
      "try{C.compose(p,null,o)}catch(e){if(e.code!=='INVALID_ARGUMENT')throw e;caught=true}"
      "if(!caught)throw Error('core failure missing');for(let i=0;i<12;i++)if(o[i]!==37)throw Error('core failure changed output');"
      "p[6]=1;C.compose(p,null,o);if(o[3]!==1||o[7]!==2||o[11]!==3)throw Error('core recovery');"
      "})();"
      "(()=>{const C=pocket.ayatsuri,c=new Float32Array([1,2,56.8489,0,10.5558,0,31.9775,3.9584]),"
      "p=new Float64Array(14),s=new Float32Array(4),g=new Float32Array([.15,.15]),f=new Float32Array(2);"
      "C.followPrepare(c,p);C.followStep(p,s,g,f);__coreAlloc();"
      "for(let i=0;i<1200;i++)C.followStep(p,s,g,f);__coreAlloc(1);"
      "const phase=new Uint32Array([0,4294967295,123]),inc=new Uint32Array([1,4294967295,1073741824]),o=new Float32Array(1);"
      "C.followDrive(phase,inc,o);__coreAlloc();for(let i=0;i<1200;i++)C.followDrive(phase,inc,o);__coreAlloc(1);"
      "const before=new Float32Array(s);f[1]=NaN;let caught=false;"
      "try{C.followStep(p,s,g,f)}catch(e){caught=e.code==='INVALID_ARGUMENT'}"
      "if(!caught)throw Error('follow invalid missing');for(let k=0;k<4;k++)if(s[k]!==before[k])throw Error('follow partial publish');"
      "f[1]=0;C.followStep(p,s,g,f);"
      "const actor=C.followCreate(c,new Float32Array(6)),view={position:new Float32Array(2),velocity:new Float32Array(2),goal:new Float32Array(2)};"
      "actor.retarget(g);actor.step(f);actor.sample(view);__coreAlloc();for(let i=0;i<1200;i++){actor.retarget(g);actor.step(f);actor.sample(view)}__coreAlloc(1);"
      "actor.sample(view);const q=view.position[0],v=view.velocity[0];actor.retarget([.1,-.1]);actor.sample(view);"
      "if(view.position[0]!==q||view.velocity[0]!==v)throw Error('managed retarget reset');"
      "let busy=false;try{actor.retarget({length:2,get 0(){actor.reset();return 0},1:0})}catch(e){busy=true}"
      "if(!busy)throw Error('managed reentry missing');actor.step(f);"
      "const driver=C.driveCreate(42),ds={phases:new Uint32Array(3),increments:new Uint32Array(3)};driver.snapshot(ds);"
      "if(ds.phases[0]!==1083814273||ds.phases[1]!==378494188||ds.phases[2]!==2479403867)throw Error('managed seed');"
      "driver.step();driver.reset();driver.snapshot(ds);if(ds.phases[0]!==1083814273)throw Error('managed phase reset');"
      "const wide=new Float32Array([1,1,0,0,0,0,1e18,0]),sentinel=new Float64Array(14);sentinel.fill(37);"
      "let bounded=false;try{C.followPrepare(wide,sentinel)}catch(e){bounded=e.code==='LIMIT_EXCEEDED'}"
      "if(!bounded)throw Error('condition rejection missing');for(let i=0;i<14;i++)if(sentinel[i]!==37)throw Error('condition partial prepare');"
      "actor.sample(view);const oldState=new Float32Array([view.position[0],view.position[1],view.velocity[0],view.velocity[1]]);"
      "bounded=false;try{C.followCreate({inertia:[1,1],stiffness:[0,0],damping:[0,0],couplingStiffness:1e18})}catch(e){bounded=e.code==='LIMIT_EXCEEDED'}"
      "if(!bounded)throw Error('managed condition rejection missing');actor.sample(view);"
      "if(view.position[0]!==oldState[0]||view.position[1]!==oldState[1]||view.velocity[0]!==oldState[2]||view.velocity[1]!==oldState[3])throw Error('condition damaged existing actor');"
      "C.followPrepare(c,sentinel);actor.step(f);"
      "})();delete globalThis.__coreAlloc;";
    const bool ok=eval(script,strlen(script),"core-contract.js");
    printf("AYATSURI_CORE warm_compose_calls=1000 warm_packed_compose_calls=1000 warm_follow_calls=1200 warm_drive_calls=1200 warm_managed_pipelines=1200 last_guest_alloc_calls=%llu FLT_EVAL_METHOD=%d %s\n",
           core_loop_alloc_calls,FLT_EVAL_METHOD,ok?"PASS":"FAIL");
    return ok;
}

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
      "const packed=()=>{const b=new ArrayBuffer(148),f=new Float32Array(b,0,34);"
      "f.set(screen);f.set(normal,8);f.set(material,17);f.set(texture,25);new Uint16Array(b,136,5).set(surface);return b};"
      "const mustThrow=(f,code)=>{try{f()}catch(e){if(e.code===code)return;throw e}throw Error('missing '+code)};"
      "const resource=H.resource({maxSegments:1024});"
      "pocket.kasane.replace(tx=>{tx.background(0x000000ff);tx.image({resource,bounds:[0,0,240,135]})});"
      "H.beginFrame(0);draw();surface[0]=0x001f;H.commit();"
      "return {check:()=>{"
      "H.beginFrame(0);screen[0]=NaN;mustThrow(draw,'INVALID_ARGUMENT');screen[0]=10;"
      "normal[8]=0;mustThrow(draw,'INVALID_ARGUMENT');normal[8]=1;"
      "surface[4]=5;mustThrow(draw,'INVALID_ARGUMENT');"
      "surface[4]=4;texture[0]=0;mustThrow(draw,'INVALID_ARGUMENT');texture[0]=1;surface[4]=0;"
      "mustThrow(()=>S.ellipsoid(screen,normal,light,material,surface,new Float32Array(8)),'INVALID_ARGUMENT');"
      "const b=packed();for(const offset of [-1,.5,NaN,Infinity,4294967296,1])"
      "mustThrow(()=>S.ellipsoidPacked(b,offset,light),'INVALID_ARGUMENT');"
      "mustThrow(()=>S.ellipsoidPacked(new ArrayBuffer(147),0,light),'INVALID_ARGUMENT');"
      "mustThrow(()=>S.ellipsoidPacked(new Uint8Array(b),0,light),'INVALID_ARGUMENT');"
      "mustThrow(()=>S.ellipsoidPacked(b,0,new Float32Array(8)),'INVALID_ARGUMENT');"
      "new Float32Array(b)[0]=NaN;mustThrow(()=>S.ellipsoidPacked(b,0,light),'INVALID_ARGUMENT');"
      "new Float32Array(b)[0]=10;light[0]=NaN;mustThrow(()=>S.ellipsoidPacked(b,0,light),'INVALID_ARGUMENT');light[0]=0;"
      "S.ellipsoidPacked(b,0,light);"
      "screen[0]=120;screen[5]=67;draw();draw();mustThrow(draw,'LIMIT_EXCEEDED');"
      "mustThrow(()=>H.commit(),'BUSY');H.beginFrame(0);screen[0]=10;screen[5]=10;draw();H.commit();"
      "},light:(front)=>{material[0]=.25;material[1]=.75;light[0]=front?0:-1;light[2]=front?1:0;H.beginFrame(0);draw();H.commit()},"
      "uv:(offset)=>{surface[1]=0xf800;surface[4]=2;texture[5]=offset;texture[7]=0;texture[8]=.4;H.beginFrame(0);draw();H.commit()},"
      "packed:(offset)=>{const b=new ArrayBuffer(offset+148),raw=new Uint8Array(b);raw.set(new Uint8Array(packed()),offset);"
      "raw[offset+146]=73;raw[offset+147]=91;H.beginFrame(0);S.ellipsoidPacked(b,offset,light);raw.fill(0);H.commit()}"
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
    static uint16_t packed_reference[240*135];memcpy(packed_reference,panel,sizeof panel);
    for(unsigned offset=0;offset<2;offset++){
        const char *call=offset?"__ellipsoidTest.packed(1)":"__ellipsoidTest.packed(0)";
        if(!eval(call,strlen(call),"ellipsoid-packed.js"))return false;
        present(7+offset);
        if(memcmp(packed_reference,panel,sizeof panel)||!full_reference(7+offset))return false;
    }
    printf("ELLIPSOID_PACKED aligned/unaligned/parity/snapshot/padding/span/validation PASS\n");
    const char *cleanup="delete globalThis.__ellipsoidTest;";
    return eval(cleanup,strlen(cleanup),"ellipsoid-contract.js");
}

static bool blend_primitive_contract(void){
    const char *script=
      "globalThis.__blendTest=(()=>{"
      "const H=pocket.kasane.procedural,S=pocket.hone;"
      "const row=[10,0,0,116,0,10,0,67,.1,0,0,0,.1,0,0,0,.1,0,0,10,0];"
      "const lobes=new Float32Array(row.concat(row));lobes[24]=124;"
      "const light=new Float32Array([0,0,1,0,0,1,1,1,1]);"
      "const material=new Float32Array([1,0,0,16,0,1,1,1,1,1,1]);"
      "const surface=new Uint16Array([0xf800,255,128]),controls=new Float32Array([6,.08]);"
      "const draw=()=>S.blendEllipsoids(lobes,light,material,surface,controls);"
      "const plane=new Float32Array([1,0,0,-120,0]),color=new Uint16Array([0x001f]);"
      "const region=()=>S.blendEllipsoids(lobes,light,material,surface,controls,plane,color);"
      "const mustThrow=(f,code)=>{try{f()}catch(e){if(e.code===code)return;throw e}throw Error('missing '+code)};"
      "const resource=H.resource({maxSegments:1024});"
      "pocket.kasane.replace(tx=>{tx.background(0x000000ff);tx.image({resource,bounds:[0,0,240,135]})});"
      "H.beginFrame(0);draw();surface[0]=0x001f;H.commit();"
      "return {check:()=>{H.beginFrame(0);"
      "lobes[21]=NaN;mustThrow(draw,'INVALID_ARGUMENT');lobes[21]=10;"
      "lobes[40]=0;mustThrow(draw,'INVALID_ARGUMENT');lobes[40]=10;"
      "material[10]=2;mustThrow(draw,'INVALID_ARGUMENT');material[10]=1;"
      "controls[0]=-1;mustThrow(draw,'INVALID_ARGUMENT');controls[0]=6;"
      "surface[1]=256;mustThrow(draw,'INVALID_ARGUMENT');surface[1]=255;"
      "mustThrow(()=>S.blendEllipsoids(new Float32Array(22),light,material,surface,controls),'INVALID_ARGUMENT');"
      "mustThrow(()=>S.blendEllipsoids(new Float32Array(21*9),light,material,surface,controls),'LIMIT_EXCEEDED');"
      "mustThrow(()=>S.blendEllipsoids(lobes,light,material,new Uint8Array(3),controls),'INVALID_ARGUMENT');"
      "draw();const huge=new Float32Array(21*8);for(let i=0;i<8;i++){huge.set(row,21*i);huge[21*i]=120;huge[21*i+5]=67}"
      "mustThrow(()=>S.blendEllipsoids(huge,light,material,surface,controls),'LIMIT_EXCEEDED');"
      "mustThrow(()=>H.commit(),'BUSY');H.beginFrame(0);draw();H.commit();"
      "},ambient:(blue)=>{surface[0]=0xffff;material[8]=blue?.25:1;material[9]=blue?.5:1;H.beginFrame(0);draw();H.commit()},"
      "region:(mode)=>{surface[0]=0xf800;material[8]=material[9]=1;plane.set([1,0,0,mode===1?-110:-120,0]);color[0]=mode===2?0xf800:0x001f;H.beginFrame(0);"
      "for(let i=0;i<5;i++){const old=plane[i];plane[i]=NaN;mustThrow(region,'INVALID_ARGUMENT');plane[i]=old}"
      "plane[0]=0;mustThrow(region,'INVALID_ARGUMENT');plane[0]=1;plane[4]=1e-5;mustThrow(region,'INVALID_ARGUMENT');plane[4]=0;"
      "mustThrow(()=>S.blendEllipsoids(lobes,light,material,surface,controls,new Uint8Array(20),color),'INVALID_ARGUMENT');"
      "region();const huge=new Float32Array(21*8);for(let i=0;i<8;i++){huge.set(row,21*i);huge[21*i]=120;huge[21*i+5]=67}"
      "mustThrow(()=>S.blendEllipsoids(huge,light,material,surface,controls,plane,color),'LIMIT_EXCEEDED');"
      "plane.fill(NaN);color[0]=0;H.commit()}"
      "}})();";
    if(!eval(script,strlen(script),"blend-contract.js"))return false;
    present(0);if(panel[67*240+120]!=0xf800)return false;
    const char *check="__blendTest.check();";
    if(!eval(check,strlen(check),"blend-contract.js"))return false;
    present(1);if(panel[67*240+120]!=0x001f||!full_reference(1))return false;
    const char *blue="__blendTest.ambient(true);",*white="__blendTest.ambient(false);";
    if(!eval(blue,strlen(blue),"blend-ambient.js"))return false;
    present(2);if(panel[67*240+120]!=0x441f||!full_reference(2))return false;
    if(!eval(white,strlen(white),"blend-ambient.js"))return false;
    present(3);if(panel[67*240+120]!=0xffff||!full_reference(3))return false;
    if(!eval(blue,strlen(blue),"blend-repair.js"))return false;
    pocket_kasane_invalidate();failure_strip=0;
    ksn_display_port failing={.strip=get_strip,.present=fail_second_strip,
        .width=240,.height=135,.strip_rows=8,.text=&ksn_font_port};
    ksn_render_stats stats;
    if(pocket_kasane_present(&failing,&stats)!=KSN_IO)return false;
    const char *busy="(()=>{try{pocket.kasane.procedural.beginFrame(0)}catch(e){if(e.code==='BUSY')return;throw e}throw Error('missing repair BUSY')})()";
    if(!eval(busy,strlen(busy),"blend-repair.js"))return false;
    present(4);if(!full_reference(4))return false;
    for(unsigned mode=0;mode<3;mode++){
        char call[48];snprintf(call,sizeof call,"__blendTest.region(%u);",mode);
        if(!eval(call,strlen(call),"blend-region.js"))return false;
        present(5+mode);if(!full_reference(5+mode))return false;
        const uint16_t left=mode==1?0x001f:0xf800,right=mode==2?0xf800:0x001f;
        if(panel[67*240+116]!=left||panel[67*240+124]!=right)return false;
    }
    const char *cleanup="delete globalThis.__blendTest;";
    const bool ok=eval(cleanup,strlen(cleanup),"blend-contract.js");
    printf("BLEND_PRIMITIVE immutable/validation/budget/ambient-damage/region-snapshot/region-dirty/repair %s\n",ok?"PASS":"FAIL");
    return ok;
}

static bool native_range_contract(void){
    const char *setup=
      "globalThis.__rangeTest=(()=>{"
      "const H=pocket.kasane.procedural;let scene,part,group;"
      "return {bad:(kind)=>{"
      "scene=Ayatsuri.scene({backend:pocket.hone});"
      "group=kind===5?scene.add({name:'parent',geometry:null,scale:[1e30,1e30,1e30]}):null;"
      "part=scene.add({name:'range',parent:group,geometry:{kind:'ellipsoid',radii:[8,8,8]},"
      "material:{color:0x44aa55,ambient:1,diffuse:0},"
      "pattern:kind===4?{kind:'checker',color:0xffffff,repeats:1e308}:undefined});"
      "if(kind===0||kind===2)scene.camera({zoom:kind===0?1e4:1e38});"
      "if(kind===1||kind===3){const scale=kind===1?1e4:1e38;part.place({scale:[scale,scale,scale]})}"
      "if(kind===5)part.place({scale:[1e30,1e30,1e30]});"
      "try{scene.evaluate(0);H.beginFrame(0);scene.render()}catch(e){"
      "if(e.code!==(kind===5?'LIMIT_EXCEEDED':'INVALID_ARGUMENT'))throw e;"
      "if(kind===5){try{scene.render()}catch(reentry){return}throw Error('failed core was renderable')}return}"
      "throw Error('native accepted oversized packet');"
      "},good:()=>{"
      "scene.camera({zoom:1});part.place({scale:[1,1,1]});"
      "if(group)group.place({scale:[1,1,1]});"
      "part.texture({scale:[0,0],offset:[-.5,-.5]});"
      "scene.evaluate(0);H.beginFrame(0);scene.render();H.commit();scene.dispose();"
      "}}})();";
    if(!eval(setup,strlen(setup),"native-range.js"))return false;
    static uint16_t previous[240*135];
    for(unsigned kind=0;kind<6;++kind){
        memcpy(previous,panel,sizeof previous);
        char call[40];snprintf(call,sizeof call,"__rangeTest.bad(%u)",kind);
        if(!eval(call,strlen(call),"native-range.js"))return false;
        pocket_kasane_invalidate();present(0);
        if(memcmp(previous,panel,sizeof previous))return false;
        const char *recover="__rangeTest.good();";
        if(!eval(recover,strlen(recover),"native-range.js"))return false;
        present(1);
        if(panel[75*240+120]!=0x454a||!full_reference(1))return false;
    }
    const char *cleanup="delete globalThis.__rangeTest;";
    return eval(cleanup,strlen(cleanup),"native-range.js");
}


/* Diagnostic branch uses the app's own clock controller and real native present.
 * Captures are host simulation, never LCD/device evidence. */
static unsigned app_model_count;
static bool capture_call(unsigned model,unsigned mode,unsigned tick){
    char call[256];
    snprintf(call,sizeof call,
        "AyatsuriLab.control.set({model:%u,mode:%u,tick:%u,breathing:false,paused:true});frame(0)",
        model,mode,tick);
    if(!eval(call,strlen(call),"capture-seek.js"))return false;
    present((uint64_t)tick*1000000u/30u);
    return num("AyatsuriLab.control.read({}).lastSubmittedTick")==tick &&
           num("AyatsuriLab.control.read({}).tick")==tick &&
           num("AyatsuriLab.control.read({}).lastSubmittedModel")==model &&
           num("AyatsuriLab.control.read({}).lastSubmittedMode")==mode &&
           num("AyatsuriLab.control.read({}).lastSubmittedBreathing")==0;
}
static bool capture_clock_contract(void){
    static uint16_t expected[240*135];
    if(!capture_call(0,0,82))return false;
    memcpy(expected,panel,sizeof expected);
    if(!capture_call(0,0,90)||!capture_call(0,0,20)||!capture_call(0,0,82))return false;
    if(memcmp(expected,panel,sizeof expected))return false;
    if(!eval("frame(0)",8,"capture-pause.js"))return false;
    present(1);
    if(memcmp(expected,panel,sizeof expected)||num("AyatsuriLab.control.read({}).tick")!=82)return false;
    const char *step="AyatsuriLab.control.step();frame(0)";
    if(!eval(step,strlen(step),"capture-step.js"))return false;
    present(2);
    if(num("AyatsuriLab.control.read({}).tick")!=83||
       num("AyatsuriLab.control.read({}).lastSubmittedTick")!=83||
       num("AyatsuriLab.control.read({}).pendingSteps")!=0)return false;
    if(!eval("frame(0)",8,"capture-step-paused.js"))return false;
    present(3);
    if(num("AyatsuriLab.control.read({}).tick")!=83||
       num("AyatsuriLab.control.read({}).lastSubmittedTick")!=83)return false;
    const char *resume="AyatsuriLab.control.pause(false);frame(0)";
    if(!eval(resume,strlen(resume),"capture-resume.js"))return false;
    present(4);
    if(num("AyatsuriLab.control.read({}).tick")!=84||
       num("AyatsuriLab.control.read({}).lastSubmittedTick")!=83)return false;
    const char *pause="AyatsuriLab.control.pause()";
    if(!eval(pause,strlen(pause),"capture-pause.js"))return false;
    for(unsigned kind=0;kind<3;++kind){
        printf("CHARACTER_CAPTURE injection_stage=%s baseline_tick=90 candidate_tick=91\n",
            kind==0?"evaluate":kind==1?"render":"commit");
        if(!capture_call(0,0,90))return false;
        memcpy(expected,panel,sizeof expected);
        char script[1200];
        snprintf(script,sizeof script,
          "(()=>{const C=AyatsuriLab.control,o=%s,k0='%s',k=k0==='compose'&&typeof o.composeNode==='function'?'composeNode':k0==='ellipsoid'&&typeof o.ellipsoidPacked==='function'?'ellipsoidPacked':k0,f=o[k],s={};"
          "C.step();let calls=0,caught=false;o[k]=()=>{++calls;throw Error('injected native submission failure')};"
          "try{frame(0)}catch(e){caught=true}finally{o[k]=f}"
          "C.read(s);if(!caught||calls!==1||s.tick!==90||s.pendingSteps!==1||s.lastSubmittedTick!==90)throw Error('failure clock calls='+calls+' tick='+s.tick+' credits='+s.pendingSteps+' last='+s.lastSubmittedTick);"
          "})();",kind==0?"pocket.ayatsuri":kind==1?"pocket.hone":"pocket.kasane.procedural",
          kind==0?"compose":kind==1?"ellipsoid":"commit");
        if(!eval(script,strlen(script),"capture-failure.js"))return false;
        pocket_kasane_invalidate();present(2);
        if(memcmp(expected,panel,sizeof expected))return false;
        if(!eval("frame(0)",8,"capture-recovery.js"))return false;
        present(3);
        if(num("AyatsuriLab.control.read({}).tick")!=91||num("AyatsuriLab.control.read({}).lastSubmittedTick")!=91)return false;
        memcpy(expected,panel,sizeof expected);
        if(!capture_call(0,0,91)||memcmp(expected,panel,sizeof expected))return false;
    }
    printf("CHARACTER_CAPTURE clock_seek_reverse_pause_step_resume=PASS evaluate_render_commit_failure_recovery=PASS\n");
    return true;
}
static bool capture_images(void){
    const char *dir=getenv("CHARACTER_CAPTURE_DIR"),*plan=getenv("CHARACTER_CAPTURE_PLAN");
    if(!dir||!plan)return false;
    FILE *f=fopen(plan,"r");if(!f)return false;
    bool ok=capture_clock_contract();
    unsigned model,mode,tick,count=0;char name[180];
    static uint16_t expected[240*135];
    while(ok&&fscanf(f,"%u %u %u %179s",&model,&mode,&tick,name)==4){
        if(model>=app_model_count||mode>5||tick>1000000000u||strchr(name,'/')||strchr(name,'\\')){ok=false;break;}
        ok=capture_call(model,mode,tick);
        if(!ok)break;
        memcpy(expected,panel,sizeof expected);
        /* Change the requested sample, then return: no history-sensitive poses. */
        ok=capture_call(model,mode,tick?tick-1:1)&&capture_call(model,mode,tick);
        if(!ok||memcmp(expected,panel,sizeof expected)){ok=false;break;}
        if(!full_reference((uint64_t)tick*1000000u/30u)){ok=false;break;}
        char path[1024];
        if(snprintf(path,sizeof path,"%s/%s",dir,name)>=(int)sizeof path){ok=false;break;}
        FILE *image=fopen(path,"wb");if(!image){ok=false;break;}
        fprintf(image,"P6\n240 135\n255\n");
        for(size_t i=0;i<240*135;++i){
            const uint16_t p=panel[i];
            unsigned char rgb[3]={(unsigned char)((((p>>11)&31u)*255u+15u)/31u),
                (unsigned char)((((p>>5)&63u)*255u+31u)/63u),(unsigned char)(((p&31u)*255u+15u)/31u)};
            if(fwrite(rgb,1,3,image)!=3){ok=false;break;}
        }
        if(fclose(image))ok=false;
        printf("HOST SIMULATION capture=%u model=%u mode=%u tick=%u breathing=false\n",count++,model,mode,tick);
        if(getenv("CHARACTER_NO_EAGER_GC")==NULL)JS_RunGC(rt);
    }
    if(!feof(f))ok=false;
    fclose(f);
    ok=ok&&count>0&&!exceptions&&!present_failures;
    printf("CHARACTER_CAPTURE allocator_peak=%zu max_request=%zu live=%zu\n",peak_bytes,largest_guest_request,heap_used());
    pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
    ok=ok&&cur_bytes==0;
    printf("CHARACTER_CAPTURE frames=%u released_guest=%zu %s\n",count,cur_bytes,ok?"CHARACTER_CAPTURE_PASS":"CHARACTER_CAPTURE_FAIL");
    return ok;
}


/* This bounded path only sends real frame buttons. No diagnostics controller,
 * arbitrary JS control script, sleeps, or per-frame forced GC are involved. */
static uint32_t fixture_panel_hash=UINT32_C(2166136261);
static unsigned fixture_presented;
static void fixture_record_presented(void){
    for(unsigned i=0;i<240*135;++i){
        fixture_panel_hash=(fixture_panel_hash^(panel[i]&255u))*UINT32_C(16777619);
        fixture_panel_hash=(fixture_panel_hash^(panel[i]>>8))*UINT32_C(16777619);
    }
    ++fixture_presented;
}
static bool fixture_real_frame(unsigned buttons){
    if(fixture_presented>=360)return false;
    char call[32];snprintf(call,sizeof call,"frame(%u)",buttons);
    if(!eval(call,strlen(call),"fixture-frame.js")){
        JSOOMCanary canary;JS_TakeOOMCanary(rt,&canary);size_t used=0,limit=0;JS_GetMemoryCounters(rt,&used,&limit);
        fprintf(stderr,"CHARACTER_FIXTURE_FRAME_FAILED buttons=%u presented=%u used=%zu limit=%zu quota_rejections=%u allocator_rejections=%u first_req=%zu first_used=%zu\n",
            buttons,fixture_presented,used,limit,canary.quota_count,canary.allocator_count,canary.first_req,canary.first_used);
        return false;
    }
    /* The app can catch a quota failure and return normally. Preserve that
     * evidence without allocating another JS error/string while near the cap. */
    JSOOMCanary caught;JS_TakeOOMCanary(rt,&caught);
    if(caught.quota_count||caught.allocator_count){
        size_t used=0,limit=0;JS_GetMemoryCounters(rt,&used,&limit);fixture_saw_oom=true;
        fprintf(stderr,"CHARACTER_FIXTURE_CAUGHT_OOM presented=%u used=%zu limit=%zu quota=%u allocator=%u first_req=%zu first_used=%zu\n",
            fixture_presented,used,limit,caught.quota_count,caught.allocator_count,caught.first_req,caught.first_used);
    }
    const unsigned before=present_failures;present((uint64_t)fixture_presented*33333u);
    const bool valid=present_failures==before&&full_reference((uint64_t)fixture_presented*33333u);
    fixture_record_presented();return valid;
}

static unsigned fixture_commit_hook_calls;
static JSValue fixture_commit_throw(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;++fixture_commit_hook_calls;
    return pocket_api_throw(c,POCKET_ERR_LIMIT_EXCEEDED,"kasane.procedural.commit",
        "fixture commit injection",false,POCKET_OUTCOME_NOT_APPLIED);
}
static bool fixture_inject_commit_failure(void){
    JSValue global=JS_GetGlobalObject(ctx),pocket=JS_UNDEFINED,kasane=JS_UNDEFINED,
        procedural=JS_UNDEFINED,original=JS_UNDEFINED;
    bool valid=!JS_IsException(global),installed=false,ok=false;
    if(valid){pocket=JS_GetPropertyStr(ctx,global,"pocket");valid=!JS_IsException(pocket);}
    if(valid){kasane=JS_GetPropertyStr(ctx,pocket,"kasane");valid=!JS_IsException(kasane);}
    if(valid){procedural=JS_GetPropertyStr(ctx,kasane,"procedural");valid=!JS_IsException(procedural);}
    if(valid){original=JS_GetPropertyStr(ctx,procedural,"commit");valid=!JS_IsException(original)&&JS_IsFunction(ctx,original);}
    if(valid){
        JSValue hook=JS_NewCFunction(ctx,fixture_commit_throw,"fixtureCommitFailure",0);
        if(!JS_IsException(hook))installed=JS_SetPropertyStr(ctx,procedural,"commit",hook)>=0;
    }
    fixture_commit_hook_calls=0;
    if(installed)ok=fixture_real_frame(0);
    if(installed&&JS_SetPropertyStr(ctx,procedural,"commit",JS_DupValue(ctx,original))<0)ok=false;
    JS_FreeValue(ctx,original);JS_FreeValue(ctx,procedural);JS_FreeValue(ctx,kasane);
    JS_FreeValue(ctx,pocket);JS_FreeValue(ctx,global);
    return ok&&fixture_commit_hook_calls==1;
}
static int finite_fixture_contract(bool ok){
    const char *maximum=getenv("CHARACTER_FIXTURE_MAX_FRAMES");char *end=NULL;
    const unsigned long parsed=maximum?strtoul(maximum,&end,10):360;
    const bool valid_bound=parsed>=1&&parsed<=360&&(!maximum||(*maximum&&end&&!*end));
    unsigned fixture_frames=0;
    ok=ok&&valid_bound&&app_model_count==1;
    if(ok)ok=fixture_real_frame(0)&&fixture_starts==0;
    if(ok){ok=fixture_real_frame(0x4000);fixture_frames=1;}
    while(ok&&!fixture_completes&&!fixture_returns&&fixture_frames<parsed){
        ok=fixture_real_frame(0);++fixture_frames;
    }
    const bool completed=fixture_starts==1&&fixture_completes==1&&fixture_passes==1;
    /* Allow the normal app to process its completion transition, then send
     * U through its ordinary input path and release it. Both must render. */
    if(ok)ok=fixture_real_frame(0)&&fixture_real_frame(0);
    if(ok)ok=fixture_real_frame(0x10)&&fixture_real_frame(0);
    ok=ok&&completed&&fixture_return_complete==1&&fixture_returns==1&&fixture_starts==1;
    if(ok){
        /* Inject one prepublication failure only in this host test. The app
         * must catch it, retain the previous native picture and return BODY. */
        ok=fixture_real_frame(0x4000)&&fixture_real_frame(0)&&fixture_real_frame(0);
        static uint16_t previous[240*135];memcpy(previous,panel,sizeof previous);
        if(ok)ok=fixture_inject_commit_failure();
        if(ok)ok=!memcmp(previous,panel,sizeof previous);
        ok=ok&&fixture_return_failure==0&&fixture_starts==2&&fixture_completes==1;
        /* Failed-frame exception values have been retired before cleanup;
         * the next real frame performs the deferred ordinary return. */
        if(ok)ok=fixture_real_frame(0);
        ok=ok&&fixture_return_failure==1;
        if(ok)ok=fixture_real_frame(0);
        printf("CHARACTER_FIXTURE injected_commit_failure=%s previous_picture_unchanged=%u ordinary_recovery=%u\n",ok?"PASS":"FAIL",ok,ok);
    }
    ok=ok&&!exceptions&&!present_failures&&!fixture_saw_oom;
    printf("CHARACTER_FIXTURE_PANEL fnv32=%08x presented=%u\n",(unsigned)fixture_panel_hash,fixture_presented);
    printf("CHARACTER_FIXTURE frames=%u bound=%lu starts=%u completes=%u pass=%u returns=%u ordinary_return=%u full_reference=1 eager_gc=0 live=%zu peak_requested=%zu max_request=%zu\n",
        fixture_frames,parsed,fixture_starts,fixture_completes,fixture_passes,fixture_returns,
        fixture_return_complete,heap_used(),peak_bytes,largest_guest_request);
    pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
    if(cur_bytes)ok=false;
    printf("CHARACTER_FIXTURE released_guest=%zu\n",cur_bytes);
    printf("%s\n",ok?"CHARACTER_FIXTURE_PASS":"CHARACTER_FIXTURE_FAIL");return ok?0:1;
}

#ifdef CHARACTER_MODEL_STUDY
#ifdef CHARACTER_MODEL_BLOB
#include "model_study_blob.h"
extern JSValue pocket_ayatsuri_model_from_static(JSContext *,const uint8_t *,size_t);
static JSValue model_study_static_model(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argv;if(argc)return JS_ThrowTypeError(c,"static model takes no arguments");
    return pocket_ayatsuri_model_from_static(c,model_study_blob,sizeof model_study_blob);
}
#endif
static FILE *model_study_pixels;
static unsigned model_study_frame;
static uint64_t model_study_panel_hash(void){
    uint64_t hash=UINT64_C(14695981039346656037);
    for(size_t i=0;i<240*135;i++){
        hash^=panel[i]&255u;hash*=UINT64_C(1099511628211);
        hash^=panel[i]>>8;hash*=UINT64_C(1099511628211);
    }return hash;
}
static bool model_study_script(const char *script,const char *phase){
    const bool ok=eval(script,strlen(script),"model-study-phase.js");character_memory_record(phase,false);return ok;
}
static bool model_study_tick(unsigned slot,unsigned tick){
    JSValue global=JS_GetGlobalObject(ctx),object=JS_GetPropertyStr(ctx,global,"__modelStudy"),
      function=JS_GetPropertyStr(ctx,object,"frame"),args[2]={JS_NewUint32(ctx,slot),JS_NewUint32(ctx,tick)};
    JS_FreeValue(ctx,global);JSValue result=JS_Call(ctx,function,object,2,args);
    const bool ok=!JS_IsException(result);
    if(!ok){JSValue error=JS_GetException(ctx);const char *text=JS_ToCString(ctx,error);
        fprintf(stderr,"MODEL_STUDY_FRAME_FAIL slot=%u tick=%u %s\n",slot,tick,text?text:"null");
        JS_FreeCString(ctx,text);JS_FreeValue(ctx,error);++exceptions;}
    JS_FreeValue(ctx,result);JS_FreeValue(ctx,function);JS_FreeValue(ctx,object);
    JSContext *pending;while(JS_ExecutePendingJob(rt,&pending)>0){}
    JS_VMStackTrim(rt);pocket_kasane_end_turn();
    if(ok){
        const uint64_t time=(uint64_t)tick*1000000u/30u;present(time);
        if(getenv("CHARACTER_FULL_REFERENCE"))return full_reference(time)&&
          (!model_study_pixels||fprintf(model_study_pixels,"%u,%u,%u,%016llx\n",model_study_frame++,slot,tick,(unsigned long long)model_study_panel_hash())>0);
    }return ok;
}
static int model_study_run(bool ok){
    const char *driver=getenv("CHARACTER_MODEL_DRIVER");
    if(!driver)return 2;
    FILE *file=fopen(driver,"rb");if(!file)return 2;
    static char script[16384];const size_t bytes=fread(script,1,sizeof script-1,file);bool read_ok=!ferror(file)&&feof(file);fclose(file);script[bytes]=0;
    ok=ok&&read_ok&&eval(script,bytes,driver);character_memory_record("driver_loaded",false);
    const char *frames_value=getenv("CHARACTER_MODEL_FRAMES");const unsigned frames=frames_value?(unsigned)strtoul(frames_value,NULL,10):301;
    if(!frames||frames>3600)ok=false;
    if(ok)ok=model_study_script("__modelStudy.create(0)","instance1_created");
    if(ok){ok=model_study_tick(0,0);character_memory_record("instance1_first_frame",false);}
    for(unsigned tick=1;ok&&tick<frames;++tick)ok=model_study_tick(0,tick);
    character_memory_record("instance1_steady",false);
    bool second=false;
    if(ok){
        ok=model_study_script("__modelStudy.trySecond()","two_instances_attempted");
        if(ok){
        second=ok&&num("__studySecondOk")==1;
        JSValue global=JS_GetGlobalObject(ctx),study=JS_GetPropertyStr(ctx,global,"__modelStudy"),
          reason=JS_GetPropertyStr(ctx,study,"secondReason");
        const char *text=JS_ToCString(ctx,reason);
        if(!text)ok=false;
        printf("MODEL_STUDY_TWO_INSTANCES result=%s reason=%s\n",second?"PASS":"CONSTRUCTION_REJECTED",text?text:"reason_unavailable");
        JS_FreeCString(ctx,text);JS_FreeValue(ctx,reason);JS_FreeValue(ctx,study);JS_FreeValue(ctx,global);
        if(ok){
        if(second){ok=model_study_tick(1,0);character_memory_record("instance2_first_frame",false);}
        else {const uint64_t before=model_study_panel_hash();ok=model_study_tick(0,frames-1)&&before==model_study_panel_hash();character_memory_record("failed_second_survivor_unchanged",false);}
        }
        }
    }
    if(ok)ok=model_study_script("__modelStudy.dispose(0)","instance1_disposed_other_alive");
    if(ok&&second)ok=model_study_tick(1,1);
    if(ok)ok=model_study_script("__modelStudy.create(0)","instance1_reloaded");
    if(ok)ok=model_study_tick(0,0);
    if(ok)ok=model_study_script("__modelStudy.dispose(0);__modelStudy.dispose(1);delete globalThis.__modelStudy","instances_disposed_before_gc");
    if(ok&&getenv("CHARACTER_MODEL_GC_PROBE")){JS_RunGC(rt);character_memory_record("instances_disposed_explicit_gc_SECONDARY",true);}
    pocket_kasane_reset();character_memory_record("presenter_reset",false);
    JS_FreeContext(ctx);ctx=NULL;character_memory_record("context_released",false);
    JS_FreeRuntime(rt);rt=NULL;character_memory_record("runtime_released",false);
    ok=ok&&!cur_bytes&&!character_memory_live_blocks&&!exceptions&&!present_failures&&!study_native_overflow;
    character_memory_close();if(model_study_pixels&&fclose(model_study_pixels))ok=false;
    model_study_pixels=NULL;ok=ok&&!character_memory_failed;
    printf("MODEL_STUDY_%s frames=%u guest_released=%zu native_requested_current=%zu native_requested_peak=%zu observer_registry_bytes=%zu\n",
        ok?"PASS":"FAIL",frames,cur_bytes,study_native_current,study_native_peak,sizeof study_native_blocks);
    return ok?0:1;
}
#endif

static uint64_t memory_panel_hash(void){
    uint64_t h=UINT64_C(14695981039346656037);
    for(unsigned i=0;i<240*135;++i){h=(h^(panel[i]&255u))*UINT64_C(1099511628211);h=(h^(panel[i]>>8))*UINT64_C(1099511628211);}
    return h;
}
static unsigned memory_frames;
static bool memory_frame(unsigned buttons){
    char script[32];snprintf(script,sizeof script,"frame(%u)",buttons);
    if(!eval(script,strlen(script),"memory-frame.js"))return false;
    JSOOMCanary oom;JS_TakeOOMCanary(rt,&oom);
    if(oom.count){size_t used=0,limit=0;JS_GetMemoryCounters(rt,&used,&limit);
        fprintf(stderr,"CLOUD_MEMORY_HOST_OOM frame=%u quota=%u allocator=%u first_req=%zu first_used=%zu used=%zu limit=%zu\n",
                memory_frames,oom.quota_count,oom.allocator_count,oom.first_req,oom.first_used,used,limit);return false;}
    present((uint64_t)memory_frames*33333u);++memory_frames;
    return !present_failures&&full_reference((uint64_t)(memory_frames-1)*33333u);
}
static int memory_app_contract(bool loaded){
    const char *choice=getenv("CHARACTER_MEMORY_APP"),*path=getenv("CHARACTER_MEMORY_PIXELS");
    const char *profile=getenv("CHARACTER_MEMORY_PROFILE");
    if(profile&&strcmp(profile,"DEVICE_SHORT")&&strcmp(profile,"HOST_LONG"))return 2;
    const bool long_profile=profile&&!strcmp(profile,"HOST_LONG");
    const unsigned expected_draws=long_profile?1115:63;
    FILE *pixels=path?fopen(path,"w"):NULL;
    bool ok=loaded&&(!path||pixels)&&eval("globalThis.__memoryRead={}",26,"memory-read.js");
    if(ok)ok=memory_frame(0)&&num("AyatsuriCloudMemory.read(__memoryRead).variant===null")==1;
    if(ok)ok=memory_frame(*choice=='A'?0x80:0x20)&&memory_frame(0);
    for(unsigned repeat=0;ok&&repeat<2;++repeat){
        unsigned last=0,turn=0;ok=memory_frame(0x4000);
        while(ok&&turn++<1300){
            const unsigned count=(unsigned)num("AyatsuriCloudMemory.read(__memoryRead).draws");
            if(count>last){
                if(count!=last+1){ok=false;break;}
                if(pixels)fprintf(pixels,"%u,%u,%016llx\n",repeat,count,(unsigned long long)memory_panel_hash());
                last=count;
            }
            if(num("AyatsuriCloudMemory.read(__memoryRead).phase==='COMPLETE'")==1)break;
            ok=memory_frame(0);
        }
        ok=ok&&turn<1300&&last==expected_draws&&num("AyatsuriCloudMemory.read(__memoryRead).released&&__memoryRead.retained")==1;
        printf("CLOUD_MEMORY_HOST variant=%c profile=%s run=%u draws=%u turns=%u peak_requested=%zu largest_request=%zu\n",*choice,long_profile?"HOST_LONG":"DEVICE_SHORT",repeat+1,last,turn,peak_bytes,largest_guest_request);
        if(ok)ok=memory_frame(0);
    }
    if(pixels)fclose(pixels);
    pocket_kasane_reset();host_capabilities_clear();JS_FreeContext(ctx);JS_FreeRuntime(rt);ctx=NULL;rt=NULL;
    ok=ok&&!cur_bytes&&!present_failures;
    printf("CLOUD_MEMORY_HOST_%s released_guest=%zu\n",ok?"PASS":"FAIL",cur_bytes);
    return ok?0:1;
}

static bool source_plane_eval_file(const char *path){
    FILE *file=fopen(path,"rb");if(!file)return false;
    static char source[65536];const size_t n=fread(source,1,sizeof source-1,file);
    const bool complete=!ferror(file)&&feof(file);fclose(file);source[n]=0;
    return complete&&n&&eval(source,n,path);
}
static bool source_plane_call(const char *method){
    char script[100];const int n=snprintf(script,sizeof script,"__sourcePlaneFixture.%s();",method);
    return n>0&&(size_t)n<sizeof script&&eval(script,(size_t)n,"source-plane-phase.js");
}
static bool source_plane_renderer_contract(void){
    const char *files[]={"apps/character/character.js","apps/character/character_blend.js",
        "apps/lib/ayatsuri_local_plane.js","tools/character/source_plane_render_fixture.js"};
    for(unsigned k=0;k<4;k++)if(!source_plane_eval_file(files[k]))return false;
    static uint16_t same[240*135],reset[240*135];
    const char *phases[]={"plain","same","colour","pose","light","camera","hide","show","reset","snapshot","invalid"};
    for(unsigned k=0;k<sizeof phases/sizeof phases[0];k++){
        if(!source_plane_call(phases[k]))return false;
        present(k);if(!full_reference(k)||present_failures)return false;
        if(k==0)memcpy(same,panel,sizeof panel);
        if(k==1&&memcmp(same,panel,sizeof panel))return false;
        if(k==6)for(unsigned pixel=0;pixel<240*135;pixel++)if(panel[pixel])return false;
        if(k==8)memcpy(reset,panel,sizeof panel);
        if(k>=9&&memcmp(reset,panel,sizeof panel))return false;
        printf("SOURCE_PLANE_FRAME phase=%s hash=%016llx\n",phases[k],(unsigned long long)memory_panel_hash());
    }
    if(!source_plane_call("pose"))return false;
    pocket_kasane_invalidate();failure_strip=0;
    ksn_display_port failing={.strip=get_strip,.present=fail_second_strip,
        .width=240,.height=135,.strip_rows=8,.text=&ksn_font_port};
    ksn_render_stats stats;if(pocket_kasane_present(&failing,&stats)!=KSN_IO)return false;
    const char *busy="(()=>{try{pocket.kasane.procedural.beginFrame(0)}catch(e){if(e.code==='BUSY')return;throw e}throw Error('missing repair BUSY')})()";
    if(!eval(busy,strlen(busy),"source-plane-repair.js"))return false;
    present(11);if(!full_reference(11))return false;
    if(!source_plane_call("reset"))return false;
    present(12);
    if(memcmp(reset,panel,sizeof panel)||!full_reference(12))return false;
    if(!source_plane_call("dispose"))return false;
    present(13);
    if(memcmp(reset,panel,sizeof panel)||!full_reference(13))return false;
    if(!source_plane_call("recreate"))return false;
    present(14);
    if(memcmp(reset,panel,sizeof panel)||!full_reference(14))return false;
    if(!source_plane_call("cleanup"))return false;
    printf("SOURCE_PLANE_FRAME phase=recreated hash=%016llx\n",(unsigned long long)memory_panel_hash());
    const char *cleanup="delete globalThis.__sourcePlaneFixture;delete globalThis.AyatsuriLocalPlane;delete globalThis.AyatsuriBlend;delete globalThis.Ayatsuri;delete globalThis.Character;";
    return eval(cleanup,strlen(cleanup),"source-plane-cleanup.js")&&!exceptions&&!present_failures;
}
static int character_host_run(int argc,char **argv){
#ifdef CHARACTER_MODEL_STUDY
    study_native_active=getenv("CHARACTER_MEMORY_STUDY")!=NULL;
    if(study_native_active&&!study_allocator_selftest())return 2;
    const char *pixel_path=getenv("CHARACTER_MODEL_PIXELS");
    if(study_native_active&&pixel_path){++study_observer_depth;model_study_pixels=fopen(pixel_path,"w");
        if(model_study_pixels)setvbuf(model_study_pixels,NULL,_IONBF,0);
        --study_observer_depth;
        if(!model_study_pixels)return 2;}

#endif
    const char *path=getenv("CHARACTER_APP_ENTRY");if(!path)path="apps/character/character_lab.js";
    FILE *f=fopen(path,"rb");if(!f)return 2;
    char src[4096];size_t n=fread(src,1,sizeof(src)-1,f);
    if(ferror(f)||!feof(f)){fclose(f);return 2;}fclose(f);src[n]=0;
    rt=JS_NewRuntime2(&PEAK_MF,NULL);ctx=JS_NewContext(rt);host_capabilities_clear();
    const char *limit=getenv("CHARACTER_HEAP_LIMIT");
    if(limit){size_t cap=strtoul(limit,NULL,0);JS_SetMemoryLimit(rt,cap);JS_SetGCThreshold(rt,cap/2);}
#ifdef CHARACTER_MODEL_STUDY
    if(getenv("CHARACTER_MEMORY_STUDY"))character_memory_record("runtime_context",false);
#endif
    pocket_kasane_install(ctx,NULL);pocket_shape_install(ctx,NULL);
    JSValue g=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,g,"__log",JS_NewCFunction(ctx,js_log,"log",1));
    JS_SetPropertyStr(ctx,g,"__now",JS_NewCFunction(ctx,js_now,"now",0));
    JS_SetPropertyStr(ctx,g,"__load",JS_NewCFunction(ctx,js_load,"load",1));JS_FreeValue(ctx,g);
    g=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,g,"__coreAlloc",JS_NewCFunction(ctx,js_core_alloc_checkpoint,"checkpoint",1));JS_FreeValue(ctx,g);
#if defined(CHARACTER_MODEL_STUDY) && defined(CHARACTER_MODEL_BLOB)
    g=JS_GetGlobalObject(ctx);JS_SetPropertyStr(ctx,g,"__nativeCloudModelCreate",JS_NewCFunction(ctx,model_study_static_model,"nativeCloudModelCreate",0));JS_FreeValue(ctx,g);
#endif
    const char *app_chunks=getenv("CHARACTER_APP_CHUNKS");if(!app_chunks)app_chunks="apps/character/chunks.txt";
    if(setenv("MEGA_CHUNKS",app_chunks,1))return 2;
    read_chunks();
#ifdef CHARACTER_MODEL_STUDY
    if(getenv("CHARACTER_MEMORY_STUDY")){
        character_memory_record("api_installed",false);
        bool loaded=eval(PRELUDE,strlen(PRELUDE),"prelude.js");character_memory_record("prelude_loaded",false);
        if(loaded)loaded=eval(src,n,path);
        character_memory_record("app_loaded",false);
        return model_study_run(loaded);
    }
#endif
    if(getenv("CHARACTER_SOURCE_PLANE")){
        bool passed=eval(PRELUDE,strlen(PRELUDE),"prelude.js")&&source_plane_renderer_contract();
        JSOOMCanary oom;JS_TakeOOMCanary(rt,&oom);
        if(oom.count){
            printf("SOURCE_PLANE_RENDER_OOM quota=%u allocator=%u first_req=%zu first_used=%zu cap=%s\n",
                oom.quota_count,oom.allocator_count,oom.first_req,oom.first_used,limit?limit:"unlimited");
            passed=false;
        }
        pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
        passed=passed&&!cur_bytes;
        printf("SOURCE_PLANE_RENDER_%s pointer_bits=%zu cap=%s released_guest=%zu\n",passed?"PASS":"FAIL",sizeof(void*)*8,limit?limit:"unlimited",cur_bytes);
        return passed?0:1;
    }
    const bool memory_app=getenv("CHARACTER_MEMORY_APP")!=NULL;
    bool ok=eval(PRELUDE,strlen(PRELUDE),"prelude.js")&&(memory_app||(primitive_contract()&&blend_primitive_contract()&&core_transform_contract()));
    if(!memory_app)JS_RunGC(rt);
    peak_bytes=cur_bytes;largest_guest_request=0;
    if(memory_app){
        const char *profile=getenv("CHARACTER_MEMORY_PROFILE");
        if(profile&&strcmp(profile,"DEVICE_SHORT")&&strcmp(profile,"HOST_LONG"))return 2;
        if(profile){JSValue global=JS_GetGlobalObject(ctx);
            ok=ok&&JS_SetPropertyStr(ctx,global,"__cloudMemoryProfile",JS_NewString(ctx,profile))>=0;
            JS_FreeValue(ctx,global);}
    }
    ok=ok&&eval(src,n,path);
    if(memory_app)return memory_app_contract(ok);
    if(ok){
        const double count=num("AyatsuriModels.names.length");
        if(!isfinite(count)||count<1||count>3||count!=(unsigned)count)return 2;
        app_model_count=(unsigned)count;
        printf("CHARACTER_NATIVE app=%s chunks=%s models=%u\n",path,app_chunks,app_model_count);
    }
    if(getenv("CHARACTER_FIXTURE"))return finite_fixture_contract(ok);
    if(getenv("CHARACTER_CAPTURE_DIR")){
        const size_t app_peak=peak_bytes,app_live=heap_used(),app_request=largest_guest_request;
        peak_bytes=cur_bytes;largest_guest_request=0;
        const char *diagnostics="pocket.app.load('diagnostics')";
        ok=ok&&eval(diagnostics,strlen(diagnostics),"capture-diagnostics.js");
        printf("CHARACTER_CAPTURE app_compile_peak=%zu app_live=%zu app_load_max_request=%zu "
            "diagnostic_compile_peak=%zu diagnostic_live=%zu diagnostic_load_max_request=%zu "
            "host_pointer_bits=%zu heap_limit=%s\n",
            app_peak,app_live,app_request,peak_bytes,heap_used(),largest_guest_request,
            sizeof(void*)*8,limit?limit:"unlimited");
        return ok&&capture_images()?0:1;
    }
    size_t compile_peak=peak_bytes,load_request=largest_guest_request,live_max=0;unsigned frames=0;
    largest_guest_request=0;
    const unsigned per_mode=argc>1?(unsigned)strtoul(argv[1],NULL,0):301;
    const bool eager_gc=getenv("CHARACTER_NO_EAGER_GC")==NULL;
    const bool compare_full=getenv("CHARACTER_FULL_REFERENCE")!=NULL;
    printf("CHARACTER_NATIVE eager_gc=%u full_reference=%u repair_injection=PASS\n",eager_gc,compare_full);
    if(per_mode<2)return 2;
    static uint16_t stopped[240*135];
    for(unsigned actor=0;ok&&actor<app_model_count;actor++)for(unsigned mode=0;ok&&mode<6;mode++){
        for(unsigned t=0;ok&&t<per_mode;t++){
            unsigned buttons=0;
            if(t==0&&(actor||mode))buttons=mode?0x20:0x4020;
            char call[64];snprintf(call,sizeof(call),"frame(%u)",buttons);
            ok=eval(call,strlen(call),"frame.js");present((uint64_t)frames*33333u);
            if(compare_full&&!full_reference((uint64_t)frames*33333u)){
                printf("incremental/full mismatch model=%u mode=%u frame=%u\n",actor,mode,t);ok=false;
            }
            if((app_model_count==1||actor==2)&&mode==0&&t==240)memcpy(stopped,panel,sizeof stopped);
            if((app_model_count==1||actor==2)&&mode==0&&t==300&&
               num("typeof AyatsuriCloudDynamic==='undefined'?0:1")==0&&memcmp(stopped,panel,sizeof stopped)){
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
    if(ok&&num("typeof AyatsuriCloudDynamic==='undefined'?0:1")==1){
        /* The ordinary matrix above keeps endogenous drive OFF for settling.
         * Exercise the live drive separately under the same guest ceiling. */
        const unsigned alive_frames=363;
        for(unsigned t=0;ok&&t<alive_frames;++t){
            char call[64];snprintf(call,sizeof call,"frame(%u)",t==0?0x20u:t==2?0x40u:0u);
            ok=eval(call,strlen(call),"alive-frame.js");present((uint64_t)frames*33333u);
            if(compare_full&&!full_reference((uint64_t)frames*33333u))ok=false;
            ++frames;if(eager_gc)JS_RunGC(rt);
            const size_t live=heap_used();if(live>live_max)live_max=live;
        }
        printf("CHARACTER_NATIVE endogenous_drive_frames=%u result=%s\n",alive_frames,ok?"PASS":"FAIL");
    }
    printf("CHARACTER_NATIVE compile_peak=%zu live_max=%zu allocator_peak=%zu load_max_request=%zu run_max_request=%zu frames=%u host_pointer_bits=%zu\n",
           compile_peak,live_max,peak_bytes,load_request,largest_guest_request,frames,sizeof(void*)*8);
    if(ok&&app_model_count==3&&sizeof(void*)==4&&limit){
        /* The smaller cloud is resident. Force human construction to fail,
         * retaining the native picture; then restore the ordinary guest cap. */
        memcpy(stopped,panel,sizeof stopped);
        const size_t before_switch=heap_used();
        const size_t ordinary_cap=strtoul(limit,NULL,0);
        const size_t parser_budget=4096;
        size_t forced_cap=before_switch+parser_budget;
        if(forced_cap>=ordinary_cap)forced_cap=ordinary_cap-1;
        /* Leave room for the tiny try/catch script, while denying the larger
         * model construction. A boolean result proves frame, not compilation,
         * threw the exception. */
        ok=forced_cap>before_switch&&forced_cap-before_switch>=parser_budget;
        JS_SetMemoryLimit(rt,forced_cap);
        const char *attempt="(()=>{try{frame(16384)}catch(e){return true}return false})()";
        JSValue result=ok?JS_Eval(ctx,attempt,strlen(attempt),"switch-oom.js",JS_EVAL_TYPE_GLOBAL):JS_UNDEFINED;
        JS_SetMemoryLimit(rt,ordinary_cap);
        const int compile_exception=JS_IsException(result);
        const int caught_frame=JS_IsBool(result)&&JS_ToBool(ctx,result)==1;
        ok=ok&&!compile_exception&&caught_frame;
        JS_FreeValue(ctx,result);JS_RunGC(rt);present((uint64_t)frames*33333u);
        ok=ok&&memcmp(stopped,panel,sizeof stopped)==0;
        const size_t failed_switch=heap_used();
        ok=ok&&eval("frame(0)",8,"switch-retry.js");present((uint64_t)(frames+1)*33333u);
        JS_RunGC(rt);
        printf("CHARACTER_NATIVE switch_failure_recovery=%s forced_cap=%zu before=%zu caught_frame=%d compile_exception=%d failed=%zu recovered=%zu\n",
               ok?"PASS":"FAIL",forced_cap,before_switch,caught_frame,compile_exception,failed_switch,heap_used());
    }
    /* Separate geometric/native failure testing from the guest-cap gate above:
     * this phase must reach native validation rather than fail in JS allocation. */
    if(ok){
        JS_SetMemoryLimit(rt,(size_t)-1);
        ok=native_range_contract();
        printf("CHARACTER_NATIVE native_range_failure_recovery=%s cases=6 cap=unlimited\n",ok?"PASS":"FAIL");
    }
    ok=ok&&!exceptions&&!present_failures;
    pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
    if(cur_bytes){printf("guest allocator retained %zu bytes after realm release\n",cur_bytes);ok=false;}
    printf("CHARACTER_NATIVE released_guest=%zu\n",cur_bytes);
    printf("%s\n",ok?"CHARACTER_NATIVE_PASS":"CHARACTER_NATIVE_FAIL");return ok?0:1;
}
#ifndef CHARACTER_HOST_NO_MAIN
int main(int argc,char **argv){return character_host_run(argc,argv);}
#endif
