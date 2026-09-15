// End-to-end host contract for pocket.kasane with the real QuickJS and the
// real fixed-storage DS core/cache/modal/renderer.
#include "pocket_kasane.h"
#include "ui/kasane/ksn_runtime.h"
#include "text/ksn_font.h"
#include "pet/ksn_pet.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

void host_capabilities_clear(void);

static JSRuntime *rt;
static JSContext *ctx;
static unsigned failures;
static uint16_t strip_pixels[240*8];
static uint16_t panel_pixels[240*135],committed_pixels[240*135];
static unsigned transfers;
static bool fail_once;
static int fail_band=-1,invalidate_band=-1;

/* Single-shot failures let QuickJS construct/catch its OOM exception. Track
 * every guest allocation so each fresh-runtime sweep also checks leaks. */
typedef union { max_align_t align; size_t size; } allocation_header;
static long fault_after=-1;
static unsigned fault_index;
static bool fault_hit,native_fault;
static size_t live_allocations;
static bool track_native;
static long native_after=-1;
static size_t native_bytes,native_max,native_calls;
static struct { void *ptr;size_t bytes; } native_blocks[16];

void *__real_calloc(size_t count,size_t size);
void __real_free(void *ptr);
void __wrap_free(void *ptr) {
    for(unsigned i=0;i<16;i++) if(ptr&&native_blocks[i].ptr==ptr) {
        native_bytes-=native_blocks[i].bytes;native_blocks[i].ptr=NULL;break;
    }
    __real_free(ptr);
}
void *__wrap_calloc(size_t count,size_t size) {
    if(track_native) native_calls++;
    if(native_fault) { native_fault=false;return NULL; }
    if(track_native&&native_after>=0&&native_after--==0) return NULL;
    void *ptr=__real_calloc(count,size);
    if(track_native&&ptr) {
        unsigned i=0;while(i<16&&native_blocks[i].ptr)i++;
        if(i==16) abort();
        native_blocks[i].ptr=ptr;native_blocks[i].bytes=count*size;
        native_bytes+=count*size;if(count*size>native_max)native_max=count*size;
    }
    return ptr;
}
static bool allocation_fails(void) {
    if(fault_after<0) return false;
    if(fault_after--!=0) return false;
    fault_after=-1;fault_hit=true;return true;
}
static void *fault_malloc(void *opaque,size_t size) {
    (void)opaque;
    if(allocation_fails()||size>SIZE_MAX-sizeof(allocation_header)) return NULL;
    allocation_header *p=malloc(sizeof(*p)+size);
    if(!p) return NULL;
    p->size=size;live_allocations++;return p+1;
}
static void *fault_calloc(void *opaque,size_t count,size_t size) {
    if(size&&count>SIZE_MAX/size) return NULL;
    void *p=fault_malloc(opaque,count*size);
    if(p) memset(p,0,count*size);
    return p;
}
static void fault_free(void *opaque,void *ptr) {
    (void)opaque;
    if(ptr) { free((allocation_header *)ptr-1);live_allocations--; }
}
static void *fault_realloc(void *opaque,void *ptr,size_t size) {
    if(!ptr) return fault_malloc(opaque,size);
    if(!size) { fault_free(opaque,ptr);return NULL; }
    if(allocation_fails()||size>SIZE_MAX-sizeof(allocation_header)) return NULL;
    allocation_header *p=realloc((allocation_header *)ptr-1,sizeof(*p)+size);
    if(!p) return NULL;
    p->size=size;return p+1;
}
static size_t fault_size(const void *ptr) {
    return ptr?((const allocation_header *)ptr-1)->size:0;
}
static const JSMallocFunctions allocator={fault_calloc,fault_malloc,fault_free,
                                         fault_realloc,fault_size};
static JSValue arm_fault(JSContext *context,JSValueConst self,int argc,JSValueConst *argv) {
    (void)context;(void)self;(void)argc;(void)argv;
    fault_after=fault_index;return JS_UNDEFINED;
}
static JSValue disarm_fault(JSContext *context,JSValueConst self,int argc,JSValueConst *argv) {
    (void)context;(void)self;(void)argc;(void)argv;
    fault_after=-1;return JS_UNDEFINED;
}

static void check(bool ok,const char *what) {
    printf("%s %s\n",ok?"ok  ":"FAIL",what);
    if(!ok)failures++;
}
static bool run(const char *source) {
    JSValue value=JS_Eval(ctx,source,strlen(source),"<kasane-test>",JS_EVAL_TYPE_GLOBAL);
    bool ok=!JS_IsException(value);
    if(!ok) {
        JSValue error=JS_GetException(ctx);
        const char *text=JS_ToCString(ctx,error);
        printf("    threw: %s\n",text?text:"?");
        if(text)JS_FreeCString(ctx,text);
        JS_FreeValue(ctx,error);
    }
    JS_FreeValue(ctx,value);return ok;
}
static uint16_t *get_strip(void *opaque) {(void)opaque;return strip_pixels;}
static ksn_result send_strip(void *opaque,uint16_t y,uint16_t rows,
                            const uint16_t *pixels) {
    (void)opaque;
    transfers++;
    memcpy(panel_pixels+y*240,pixels,rows*240*sizeof(*pixels));
    if(y/8==invalidate_band){invalidate_band=-1;pocket_kasane_invalidate();}
    if(fail_once||y/8==fail_band){fail_once=false;return KSN_IO;}
    return KSN_OK;
}
static ksn_result present(ksn_render_stats *stats) {
    ksn_display_port port={.strip=get_strip,.present=send_strip,
                          .width=240,.height=135,.strip_rows=8,.text=&ksn_font_port};
    return pocket_kasane_present(&port,stats);
}

static void atomicity_tests(void) {
    ksn_render_stats stats;
    /* Keep an old JS draw wrapper alive while its native arena is replaced. */
    check(run("globalThis.beforeReset=newRef"),"retain an old-session reference");
    pocket_kasane_reset();
    check(run("globalThis.shape={bounds:[0,0,10,10],color:0xff0000ff};"
              "globalThis.tpl=kasane.cache.create([shape]);"
              "kasane.replace(tx=>{globalThis.expiredTx=tx;globalThis.expiredModal=tx.modal;"
              "tx.background(0x000000ff);globalThis.baseRef=tx.rect(shape);"
              "globalThis.baseInst=tx.instantiate(tpl);});"),"atomicity baseline builds");
    check(present(&stats)==KSN_OK,"atomicity baseline presents");
    check(run("globalThis.expectAbort=(action,expected)=>{"
              "let inner=false,outer=false;try{kasane.replace(tx=>{"
              "tx.background(0xffffffff);globalThis.candidate=tx.rect(shape);"
              "tx.instantiate(tpl);try{action(tx,candidate)}catch(e){"
              "inner=e!==undefined&&(!expected||e===expected);}"
              "});}catch(e){outer=true}"
              "if(!inner||!outer)throw Error('caught failure submitted');"
              "let s=kasane.stats();if(s.displayed.commands!==2||s.cache.instances!==1||"
              "s.cache.templates!==1||s.cache.commands!==1)throw Error('quota leaked');};"
              "globalThis.badActions=["
              "tx=>tx.background(),tx=>tx.background(NaN),tx=>tx.rect(),"
              "tx=>tx.rect({bounds:[0,0,1],color:1}),"
              "tx=>tx.rect({bounds:[0,0,1,1],color:-1}),"
              "tx=>tx.rect({bounds:[0,0,1,1],color:1,opacity:256}),"
              "tx=>tx.group(),(tx,r)=>tx.group(r,0,255),tx=>tx.group({},1,255),"
              "tx=>tx.instantiate({}),tx=>tx.instantiate(tpl,{offset:[1]}),"
              "tx=>tx.instantiate(tpl,{visible:1}),(tx,r)=>r.setRect(tx),"
              "(tx,r)=>r.setClip(tx,[2,0,1,1]),(tx,r)=>r.setColor(tx),"
              "(tx,r)=>r.setVisible(tx,1),tx=>beforeReset.setColor(tx,1),"
              "tx=>baseRef.setColor.call({},tx,1),tx=>baseInst.place(tx,{offset:[0,NaN]}),"
              "tx=>baseInst.place.call({},tx),tx=>baseInst.setVisible(tx,1),"
              "tx=>tx.modal.open(),tx=>tx.modal.open({backdrop:'bad'}),"
              "tx=>tx.modal.open({color:-1}),tx=>tx.modal.open({focus:-1})];"
              "for(const action of badActions)expectAbort(action);"),
          "caught validation and stale draw/instance errors abort all changes and recover quotas");
    check(!pocket_kasane_has_submission(),"caught errors leave no submitted state");
    check(run("globalThis.marker=Error('getter marker');"
              "globalThis.trap=(key,base={})=>Object.defineProperty(base,key,{get(){throw marker}});"
              "for(const key of ['bounds','clip','opacity','color'])"
              "expectAbort(tx=>tx.rect(trap(key,{...shape})),marker);"
              "for(let i=0;i<4;i++)expectAbort(tx=>tx.rect({"
              "bounds:trap(i,[0,0,10,10]),color:1}),marker);"
              "for(const key of ['offset','clip','opacity','visible']){"
              "expectAbort(tx=>tx.instantiate(tpl,trap(key)),marker);"
              "expectAbort(tx=>baseInst.place(tx,trap(key)),marker);}"
              "for(let i=0;i<2;i++)expectAbort(tx=>tx.instantiate(tpl,{"
              "offset:trap(i,[0,0])}),marker);"
              "for(const key of ['backdrop','color','focus'])"
              "expectAbort(tx=>tx.modal.open(trap(key)),marker);"
              "expectAbort((tx,r)=>r.setRect(tx,trap(0,[0,0,10,10])),marker);"
              "let threw=false;try{kasane.replace(tx=>{tx.background(0xffffffff);"
              "tx.instantiate(tpl);return trap('then')})}catch(e){threw=e===marker}"
              "if(!threw||kasane.stats().cache.instances!==1)throw Error('then getter');"),
          "throwing getters retain the original exception and roll back candidates");
    check(run("globalThis.nestedBusy=false;expectAbort(tx=>{try{tx.rect()}catch(e){}"
              "try{kasane.replace(inner=>inner.background(0xffffffff))}"
              "catch(e){nestedBusy=e.code==='BUSY'}tx.background(0x000000ff);});"
              "if(!nestedBusy)throw Error('reentrant build');"
              "kasane.patch(tx=>{baseRef.setColor(tx,0x00ff00ff);"
              "for(const action of [()=>expiredTx.background(0xffffffff),"
              "()=>expiredModal.close(),()=>baseRef.setColor(expiredTx,1),"
              "()=>baseInst.setVisible({},true)]){let closed=false;try{action()}"
              "catch(e){closed=e.code==='CLOSED'}if(!closed)throw Error('foreign tx');}});"),
          "aborted callbacks cannot reenter a build; foreign transaction tokens preserve the owner");
    check(present(&stats)==KSN_OK,"valid owner still presents after foreign token failures");
    check(run("globalThis.deadRefs=[];for(let n=0;n<100;n++){"
              "let failed=false;try{kasane.replace(tx=>{tx.background(0x000000ff);"
              "for(let i=0;i<32;i++)deadRefs.push(tx.rect(shape));"
              "try{tx.rect()}catch(e){};});}catch(e){failed=true}"
              "if(!failed)throw Error('accepted');}"
              "kasane.replace(tx=>{tx.background(0x000000ff);"
              "for(let i=0;i<32;i++)tx.rect(shape);});"),
          "100 aborted full-reference builds reclaim native slots while dead wrappers survive");
    check(present(&stats)==KSN_OK,"full reference quota remains usable after repeated aborts");
    check(run("Object.defineProperty(Object.prototype,'modal',{configurable:true,"
              "set(){throw Error('prototype setter')}});"
              "try{kasane.patch(()=>{});kasane.cancel(kasane.poll().ticket)}"
              "finally{delete Object.prototype.modal;}"
              "Object.defineProperty(Object.prototype,'status',{configurable:true,"
              "set(){throw Error('prototype setter')}});"
              "try{if(kasane.poll().status!=='DISCARDED')throw Error('own status')}"
              "finally{delete Object.prototype.status;}"),
          "adapter properties do not invoke inherited setters");
}

static void repair_tests(void) {
    pocket_kasane_reset();pocket_kasane_invalidate();
    check(!pocket_kasane_needs_present()&&!pocket_kasane_active(),
          "invalidation before APP ownership does not allocate or paint");
    check(run("globalThis.tpl=kasane.cache.create([shape]);"
              "kasane.replace(tx=>{tx.background(0x102030ff);"
              "globalThis.baseRef=tx.rect(shape);globalThis.baseInst=tx.instantiate(tpl);"
              "tx.modal.open({backdrop:'dim-live',color:0x00000080,focus:7});});"),
          "repair baseline builds cached content and modal");
    ksn_render_stats stats;
    check(present(&stats)==KSN_OK,"repair baseline presents");
    memcpy(committed_pixels,panel_pixels,sizeof(panel_pixels));
    bool all_bands=true;
    for(unsigned band=0;band<17;band++) {
        if(!run("globalThis.ticket=kasane.replace(tx=>{tx.background(0xff00ffff);"
                "tx.modal.close();globalThis.candidate=tx.rect(shape);tx.instantiate(tpl);});")) {
            all_bands=false;break;
        }
        fail_band=(int)band;transfers=0;
        if(present(&stats)!=KSN_IO||transfers!=band+1||!pocket_kasane_has_submission())
            all_bands=false;
        if(!run("kasane.cancel(ticket);if(kasane.poll().status!=='DISCARDED'||"
                "kasane.poll().reason!=='CANCELLED')throw Error('cancel');")) all_bands=false;
        if(pocket_kasane_has_submission()||!pocket_kasane_needs_present()||
           pocket_kasane_input_scope(false)!=KSN_INPUT_BLOCKED) all_bands=false;
        /* No JS callback/update from cancel through failed repair and retry. */
        transfers=0;
        if(present(&stats)!=KSN_IO||transfers!=band+1||pocket_kasane_has_submission())
            all_bands=false;
        fail_band=-1;transfers=0;
        if(present(&stats)!=KSN_OK||transfers!=17||stats.transferred_bytes!=64800||
           memcmp(panel_pixels,committed_pixels,sizeof(panel_pixels))||
           pocket_kasane_needs_present()||pocket_kasane_input_scope(false)!=KSN_INPUT_MODAL)
            all_bands=false;
        if(!run("if(kasane.poll().status!=='DISCARDED'||kasane.poll().reason!=='CANCELLED')"
                "throw Error('repair changed poll');var s=kasane.stats();"
                "if(s.displayed.commands!==3||s.cache.templates!==1||s.cache.instances!==1)"
                "throw Error('repair changed quota');"
                "kasane.patch(tx=>{baseRef.setColor(tx,0x00ff00ff);baseInst.place(tx,{offset:[4,4]})});"
                "kasane.cancel(kasane.poll().ticket);")) all_bands=false;
        if(!all_bands)break;
    }
    fail_band=-1;
    check(all_bands,"all 17 failed bands cancel and repair without JS; old refs/cache/modal/poll survive");
    /* Static apps still repaint external screen damage and indicator changes. */
    memset(panel_pixels,0x5a,sizeof(panel_pixels));pocket_kasane_invalidate();transfers=0;
    check(!pocket_kasane_has_submission()&&pocket_kasane_needs_present()&&
          present(&stats)==KSN_OK&&transfers==17&&
          memcmp(panel_pixels,committed_pixels,sizeof(panel_pixels))==0,
          "host invalidation restores a static committed screen without JS");
    pocket_kasane_invalidate();invalidate_band=16;transfers=0;
    check(present(&stats)==KSN_OK&&transfers==17&&pocket_kasane_needs_present(),
          "invalidation during final transfer survives current acknowledgement");
    transfers=0;
    check(present(&stats)==KSN_OK&&transfers==17&&!pocket_kasane_needs_present(),
          "deferred invalidation triggers exactly one more full redraw");
    transfers=0;
    check(present(&stats)==KSN_OK&&transfers==0&&stats.bands==0&&stats.transferred_bytes==0,
          "idle present returns empty stats after repair");
}

static void close_fault_runtime(void) {
    fault_after=-1;pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
    ctx=NULL;rt=NULL;
}

static bool open_fault_runtime(const char *exercise) {
    rt=JS_NewRuntime2(&allocator,NULL);ctx=rt?JS_NewContext(rt):NULL;
    if(!ctx) return false;
    host_capabilities_clear();
    if(pocket_kasane_install(ctx,NULL)!=ESP_OK) return false;
    JSValue global=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,global,"armFault",JS_NewCFunction(ctx,arm_fault,"armFault",0));
    JS_SetPropertyStr(ctx,global,"disarmFault",JS_NewCFunction(ctx,disarm_fault,"disarmFault",0));
    JS_FreeValue(ctx,global);
    /* Materialize QuickJS's lazy namespace functions before faulting adapter
     * work; a failed engine-level lazy property read occurs before C entry. */
    if(!run("kasane.features();kasane.stats();kasane.poll();"
            "globalThis.shape={bounds:[0,0,10,10],color:0xff0000ff};"
            "globalThis.placement={offset:[1,1]};"
            "globalThis.tpl=kasane.cache.create([shape]);"
            "kasane.replace(tx=>{tx.background(0x000000ff);"
            "globalThis.baseRef=tx.rect(shape);globalThis.baseInst=tx.instantiate(tpl)});"))
        return false;
    ksn_render_stats stats;
    return present(&stats)==KSN_OK&&run(exercise);
}

static void fault_sweep(const char *label,const char *exercise,bool arm_inside) {
    unsigned injected=0;bool passed=true,finished=false;
    for(fault_index=0;fault_index<256;fault_index++) {
        if(!open_fault_runtime(exercise)) { passed=false;close_fault_runtime();break; }
        JSValue global=JS_GetGlobalObject(ctx);
        JSValue function=JS_GetPropertyStr(ctx,global,"exercise");
        JS_FreeValue(ctx,global);
        fault_hit=false;fault_after=arm_inside?-1:(long)fault_index;
        JSValue result=JS_Call(ctx,function,JS_UNDEFINED,0,NULL);
        fault_after=-1;
        if(fault_hit) {
            injected++;
            if(!JS_IsException(result)||!JS_HasException(ctx)||pocket_kasane_has_submission()){
                printf("    fault index=%u exception=%d pendingException=%d submission=%d\n",fault_index,
                       JS_IsException(result),JS_HasException(ctx),pocket_kasane_has_submission());
                passed=false;
            }
            JSValue error=JS_GetException(ctx);JS_FreeValue(ctx,error);
            if(!run("let s=kasane.stats();if(s.displayed.commands!==2||s.cache.instances!==1||"
                    "s.cache.templates!==1||s.cache.commands!==1)throw Error('fault quota');"
                    "kasane.patch(tx=>baseRef.setColor(tx,0x00ff00ff));"
                    "kasane.cancel(kasane.poll().ticket);")) passed=false;
        } else {
            if(JS_IsException(result)||JS_HasException(ctx)) {
                JSValue error=JS_GetException(ctx);const char *message=JS_ToCString(ctx,error);
                printf("    non-injected failure index=%u: %s\n",fault_index,message?message:"?");
                JS_FreeCString(ctx,message);JS_FreeValue(ctx,error);passed=false;
            }
            finished=true;
        }
        JS_FreeValue(ctx,result);JS_FreeValue(ctx,function);close_fault_runtime();
        if(live_allocations){printf("    leaked=%zu at fault=%u\n",live_allocations,fault_index);passed=false;}
        if(finished||!passed) break;
    }
    printf("    %s: %u allocation failures injected\n",label,injected);
    check(passed&&finished&&injected>0,label);
}

static void allocator_tests(void) {
    fault_sweep("image resource allocation failures reserve no native slot",
                "globalThis.exercise=()=>kasane.petImage();",false);
    fault_sweep("image draw allocation failures abort the candidate",
                "globalThis.asset=kasane.petImage();globalThis.exercise=()=>kasane.replace(tx=>{tx.background(255);"
                "return tx.image({resource:asset,bounds:[0,0,32,32],scale:0.5})});",false);
    fault_sweep("text conversion and wrapper allocation failures reclaim the candidate",
                "globalThis.exercise=()=>kasane.replace(tx=>{tx.background(255);"
                "tx.text({bounds:[0,0,100,16],text:'日本語',capacity:24,color:0xffffffff})});",false);
    fault_sweep("ticket, transaction, modal and property allocation failures abort",
                "globalThis.exercise=()=>kasane.replace(tx=>tx.background(0x000000ff));",false);
    fault_sweep("draw wrapper allocation failures abort and recover refs",
                "globalThis.exercise=()=>kasane.replace(tx=>{tx.background(0x000000ff);tx.rect(shape)});",false);
    fault_sweep("instance wrapper allocation failures abort and recover instance quota",
                "globalThis.exercise=()=>kasane.replace(tx=>{tx.background(0x000000ff);tx.instantiate(tpl,placement)});",false);
    fault_sweep("caught draw allocation failure rolls back an earlier native instance",
                "globalThis.exercise=()=>kasane.replace(tx=>{tx.background(0x000000ff);"
                "tx.instantiate(tpl);armFault();try{tx.rect(shape)}catch(e){}finally{disarmFault()}});",true);
    fault_sweep("caught instance allocation failure rolls back earlier commands",
                "globalThis.exercise=()=>kasane.replace(tx=>{tx.background(0x000000ff);"
                "tx.rect(shape);armFault();try{tx.instantiate(tpl,placement)}catch(e){}finally{disarmFault()}});",true);
    fault_sweep("template wrapper failures preserve template and command quotas",
                "globalThis.definition=[shape];globalThis.exercise=()=>kasane.cache.create(definition);",false);
    fault_sweep("poll allocation failures return an exception without partial properties",
                "globalThis.exercise=()=>kasane.poll();",false);
    fault_sweep("features allocation failures clean all nested objects",
                "globalThis.exercise=()=>kasane.features();",false);
    fault_sweep("stats allocation failures clean all nested objects",
                "globalThis.exercise=()=>kasane.stats();",false);

    check(open_fault_runtime("globalThis.exercise=()=>kasane.replace(tx=>tx.background(0x000000ff));"),
          "native arena fault fixture opens");
    pocket_kasane_reset();
    JSValue global=JS_GetGlobalObject(ctx),function=JS_GetPropertyStr(ctx,global,"exercise");
    JS_FreeValue(ctx,global);native_fault=true;
    JSValue result=JS_Call(ctx,function,JS_UNDEFINED,0,NULL);
    check(!native_fault&&JS_IsException(result)&&!pocket_kasane_active()&&
          !pocket_kasane_has_submission(),"native arena OOM publishes no update");
    JSValue error=JS_GetException(ctx);JS_FreeValue(ctx,error);JS_FreeValue(ctx,result);
    result=JS_Call(ctx,function,JS_UNDEFINED,0,NULL);
    check(!JS_IsException(result)&&pocket_kasane_has_submission(),"native arena retries after allocation recovery");
    JS_FreeValue(ctx,result);JS_FreeValue(ctx,function);close_fault_runtime();
    check(live_allocations==0,"all fault-test runtimes release every guest allocation");
}

static void base_block_tests(void) {
    check(open_fault_runtime(""),"base block fixture opens");
    pocket_kasane_reset();track_native=true;
    ksn_render_stats stats;
    for(int fault=0;fault<6;fault++) {
        native_after=fault;native_max=0;
        check(run("var failed=false;try{kasane.replace(tx=>tx.rect(shape))}"
                  "catch(e){failed=e.code==='OUT_OF_MEMORY'}if(!failed)throw Error('missing OOM');"
                  "if(kasane.stats().nativeBytes!==0||kasane.stats().active)throw Error('partial state');"),
              "each base allocation failure leaves no published state");
        native_after=-1;
        check(native_bytes==0&&native_max<=3072&&!pocket_kasane_has_submission(),
              "failed base blocks reclaimed and bounded");
        check(run("kasane.replace(tx=>{tx.background(0x000000ff);globalThis.r=tx.rect(shape)});"),
              "base allocation retries successfully");
        check(present(&stats)==KSN_OK,"retried base presents");
        char accounting[160];
        snprintf(accounting,sizeof(accounting),
                 "if(kasane.stats().nativeBytes!==%zu)throw Error('base accounting');",native_bytes);
        check(run(accounting),"base stats match allocated block sizes");
        check(native_max<=3072,"all successful base allocations are bounded");
        size_t calls=native_calls;
        check(run("kasane.patch(tx=>r.setColor(tx,0xabcdef80));"),"patch uses reserved blocks");
        check(present(&stats)==KSN_OK,"patch presents from reserved blocks");
        check(native_calls==calls,"patch and present allocate no native blocks");
        pocket_kasane_reset();check(native_bytes==0,"reset releases all base blocks");
    }
    track_native=false;close_fault_runtime();
}

static void lazy_cache_tests(void) {
    check(open_fault_runtime(""),"lazy cache fixture opens");
    pocket_kasane_reset();track_native=true;
    ksn_render_stats stats;
    for(int fault=0;fault<3;fault++) {
        check(run("kasane.replace(tx=>{tx.background(0x000000ff);globalThis.r=tx.rect(shape)});"),
              "ordinary drawing builds without cache");
        check(present(&stats)==KSN_OK,"ordinary drawing presents without cache");
        size_t base=native_bytes;
        check(run("globalThis.baseBytes=kasane.stats().nativeBytes;"
                  "if(kasane.stats().cache.reservedBytes!==0)throw Error('eager cache');"
                  "globalThis.oldTicket=kasane.poll().ticket;"),"cache reservation is zero before use");
        native_max=0;native_after=fault;
        check(run("var failed=false;try{kasane.cache.create([shape])}"
                  "catch(e){failed=e.code==='OUT_OF_MEMORY'}if(!failed)throw Error('missing OOM');"
                  "if(kasane.stats().nativeBytes!==baseBytes||kasane.stats().cache.reservedBytes!==0)"
                  "throw Error('reservation leak');"),"each cache allocation failure rolls back reservation");
        native_after=-1;
        check(native_bytes==base&&native_max<=3072,"partial native blocks reclaimed and bounded");
        pocket_kasane_invalidate();
        check(present(&stats)==KSN_OK,"committed frame repairs after cache OOM");
        check(run("kasane.patch(tx=>r.setColor(tx,0xabcdef80));"
                  "kasane.cancel(kasane.poll().ticket);"
                  "globalThis.newTpl=kasane.cache.create([shape]);"
                  "var s=kasane.stats();if(s.cache.reservedBytes<=0||"
                  "s.nativeBytes!==baseBytes+s.cache.reservedBytes)throw Error('accounting');"
                  "kasane.replace(tx=>{tx.background(0x000000ff);tx.instantiate(newTpl)});"),
              "old refs survive OOM and cache retries successfully");
        check(present(&stats)==KSN_OK,"retried cache instance presents");
        check(native_max<=3072,"successful cache allocations are bounded");
        pocket_kasane_reset();check(native_bytes==0,"reset frees base and all cache blocks");
    }
    track_native=false;close_fault_runtime();
}

static void system_lifetime_tests(void) {
    check(open_fault_runtime(""),"SYSTEM coexistence fixture opens");
    pocket_kasane_reset();track_native=true;
    ksn_view *system=NULL;ksn_tx tx;ksn_ref ref;ksn_render_stats stats;
    ksn_draw d={.kind=KSN_RECT,.bounds={0,0,8,8},.clip={0,0,240,135},.opacity=255,
                .data.shape={0x00ff00ff,0,0}};
    check(ksn_runtime_system_acquire(&system)==KSN_OK,"native SYSTEM acquires without APP");
    check(ksn_view_begin(system,KSN_REPLACE,&tx)==KSN_OK&&
          ksn_view_add(system,tx,&d,&ref)==KSN_OK&&ksn_view_submit(system,tx)==KSN_OK,
          "SYSTEM builds without guest submission");
    check(present(&stats)==KSN_OK&&panel_pixels[0]==0x07e0,"SYSTEM presents without active APP");
    size_t base=native_bytes;
    native_after=0;
    check(run("var failed=false;try{kasane.replace(tx=>tx.rect(shape))}"
              "catch(e){failed=e.code==='OUT_OF_MEMORY'}if(!failed)throw Error('missing OOM');"),
          "APP adapter OOM preserves acquired SYSTEM");
    native_after=-1;
    check(native_bytes==base,"APP failure releases only its allocation");
    for(unsigned i=0;i<3;i++) {
        check(run("kasane.replace(tx=>{tx.background(0x102030ff);globalThis.oldR=tx.rect(shape)});"),
              "APP attaches beside native SYSTEM");
        if(i==0)check(present(&stats)==KSN_OK,"APP commits beside SYSTEM");
        if(i==2){fail_band=1;check(present(&stats)==KSN_IO,"APP partially transfers before exit");fail_band=-1;}
        pocket_kasane_reset();
        check(native_bytes==base&&ksn_runtime_stats(KSN_SYSTEM).displayed.commands==1,
              "APP detach preserves SYSTEM reservation and commands");
        check(present(&stats)==KSN_OK,"SYSTEM repairs after APP exit without JS");
        bool pixels=true;
        for(unsigned y=0;y<135;y++)for(unsigned x=0;x<240;x++)
            if(panel_pixels[y*240+x]!=(x<8&&y<8?0x07e0:0))pixels=false;
        check(pixels,"APP pixels vanish while all SYSTEM pixels survive");
        check(run("var failed=false;try{kasane.patch(tx=>oldR.setColor(tx,0xffffffff))}"
                  "catch(e){failed=e.code==='CLOSED'}if(!failed)throw Error('revived ref');"),
              "old JS DrawRef cannot affect reattached APP");
        pocket_kasane_reset();
    }
    close_fault_runtime();
    check(live_allocations==0,"guest heap fully released while SYSTEM survives");
    ksn_change change={.property=KSN_SET_COLOR,.value.color=0xff0000ff};
    check(ksn_view_begin(system,KSN_PATCH,&tx)==KSN_OK&&
          ksn_view_change(system,tx,ref,&change)==KSN_OK&&ksn_view_submit(system,tx)==KSN_OK,
          "SYSTEM reference remains usable after QuickJS destruction");
    check(present(&stats)==KSN_OK&&panel_pixels[0]==0xf800,"SYSTEM renders with no QuickJS runtime");
    check(ksn_runtime_shutdown()==KSN_OK&&native_bytes==0,"native shutdown releases host blocks");
    track_native=false;
}

static void primitive_tests(void) {
    check(open_fault_runtime(""),"primitive fixture opens");
    ksn_view *system=NULL;
    check(ksn_runtime_system_acquire(&system)==KSN_OK,"inspect primitive descriptors through native owner");
    check(run("var f=kasane.features();if(!f.roundRect||!f.strokeRect||!f.gradient)throw Error('features');"
              "kasane.replace(tx=>{tx.background(0x000000ff);"
              "globalThis.pr=tx.roundRect({bounds:[0.5,0.5,20.5,20.5],radius:8,color:0xff0000ff});"
              "tx.strokeRect({bounds:[24,1,44,21],width:2,color:0x00ff00ff});"
              "globalThis.pg=tx.gradient({bounds:[50,0,60,8],axis:'x',from:0xff0000ff,to:0x0000ffff});"
              "tx.rect({bounds:[-2.5,-1.5,-0.5,0.49],color:0xffffffff});});"),
          "JS exposes rounded rectangle, stroke and gradient with signed rounding");
    ksn_frame frame;ksn_frame_command cmd;
    check(ksn_core_frame(system->host->core,&frame)==KSN_OK,"primitive submission is readable");
    check(ksn_core_read(system->host->core,frame.ticket,false,KSN_APP,0,&cmd)==KSN_OK&&
          cmd.draw.kind==KSN_ROUND_RECT&&cmd.draw.bounds.x0==1&&cmd.draw.bounds.x1==21&&
          cmd.draw.data.shape.radius==8,"positive half ties round away from zero");
    check(ksn_core_read(system->host->core,frame.ticket,false,KSN_APP,1,&cmd)==KSN_OK&&
          cmd.draw.kind==KSN_STROKE&&cmd.draw.data.shape.width==2,"stroke width reaches native descriptor");
    check(ksn_core_read(system->host->core,frame.ticket,false,KSN_APP,2,&cmd)==KSN_OK&&
          cmd.draw.kind==KSN_GRADIENT&&cmd.draw.data.gradient.axis==0&&
          cmd.draw.data.gradient.to==0x0000ffff&&!cmd.draw.data.gradient.dither,
          "gradient colors, axis and default dither reach native descriptor");
    check(ksn_core_read(system->host->core,frame.ticket,false,KSN_APP,3,&cmd)==KSN_OK&&
          cmd.draw.bounds.x0==-3&&cmd.draw.bounds.y0==-2&&cmd.draw.bounds.x1==-1,
          "negative half ties round away from zero");
    ksn_render_stats stats;
    check(present(&stats)==KSN_OK&&panel_pixels[50]==0xf800&&panel_pixels[59]==0x001f,
          "JS gradient renders both exact endpoint colors");
    check(run("kasane.patch(tx=>{pr.setRect(tx,[1.4,1.4,22.5,22.5]);"
              "pg.setClip(tx,[52.5,0,57.5,8]);});"),"new primitives support existing PATCH references");
    check(present(&stats)==KSN_OK,"primitive patch presents");
    memcpy(committed_pixels,panel_pixels,sizeof(panel_pixels));
    check(run("var bad=["
              "tx=>tx.roundRect({bounds:[0,0,10,10],radius:6,color:255}),"
              "tx=>tx.strokeRect({bounds:[0,0,20,20],width:3,color:255}),"
              "tx=>tx.gradient({bounds:[0,0,20,20],from:255,to:255,axis:'z'}),"
              "tx=>tx.gradient({bounds:[0,0,20,20],from:255,to:255,dither:1}),"
              "tx=>tx.gradient({bounds:[0,0,20,20],from:255,get to(){throw Error('getter')}}),"
              "tx=>tx.rect({bounds:[NaN,0,1,1],color:255}),"
              "tx=>tx.rect({bounds:[0,0,32767.5,1],color:255})];"
              "for(var badDraw of bad){let failed=false,inner=false;try{kasane.replace(tx=>{"
              "tx.background(0xffffffff);try{badDraw(tx)}catch(e){inner=true;}})}catch(e){failed=true}"
              "if(!failed||!inner)throw Error('caught invalid primitive submitted');}"),
          "invalid primitive fields, getters and coordinate overflow poison transaction");
    pocket_kasane_invalidate();
    check(present(&stats)==KSN_OK&&!memcmp(committed_pixels,panel_pixels,sizeof(panel_pixels)),
          "rejected primitives leave committed pixels intact");
    check(run("var t=kasane.replace(tx=>{tx.background(255);tx.gradient({bounds:[0,0,20,20],from:255,to:255,"
              "radius:4,dither:true});});kasane.cancel(t);"),"rounded dither gradient can be cancelled");
    close_fault_runtime();
    check(ksn_runtime_shutdown()==KSN_OK,"primitive native owner shuts down");
}

static void text_tests(void){
    check(open_fault_runtime(""),"text fixture opens");
    ksn_view *system=NULL;check(ksn_runtime_system_acquire(&system)==KSN_OK,"inspect text through native owner");
    check(run("if(!kasane.features().text)throw Error('feature');"
              "kasane.replace(tx=>{tx.background(255);globalThis.label=tx.text({bounds:[0,7,240,23],"
              "text:'Aあ😀B',capacity:32,font:'caption',color:0xffffffff});label.setReveal(tx,3)});"),
          "JS text submits counted UTF-8 and scalar reveal");
    ksn_frame frame;ksn_frame_command cmd;ksn_render_stats stats;
    check(ksn_core_frame(system->host->core,&frame)==KSN_OK&&
          ksn_core_read(system->host->core,frame.ticket,false,KSN_APP,0,&cmd)==KSN_OK&&
          cmd.draw.kind==KSN_TEXT&&cmd.draw.data.text.bytes==9&&cmd.draw.data.text.capacity==32&&
          cmd.draw.data.text.font==KSN_CAPTION&&cmd.reveal==3&&!memcmp(cmd.text,"Aあ😀B",9),
          "native text snapshot owns bytes, capacity and reveal");
    check(present(&stats)==KSN_OK&&panel_pixels[7*240+1]==0xffff&&panel_pixels[7*240+22]==0,
          "JS text reaches coverage renderer and hides unrevealed scalar");
    check(run("kasane.patch(tx=>{label.setText(tx,'AA');label.setColor(tx,0xff0000ff)});"),
          "setText restores full reveal and accepts color patch");
    check(present(&stats)==KSN_OK&&panel_pixels[7*240+7]==0xf800,"text patch renders new bytes and color");
    memcpy(committed_pixels,panel_pixels,sizeof(panel_pixels));
    check(run("var invalid=['\\ud800','\\udc00','a\\n','a\\0',42,'あ'.repeat(43),'x'.repeat(129)];"
              "for(var value of invalid){let inner=false,outer=false;try{kasane.patch(tx=>{"
              "label.setColor(tx,0x00ff00ff);try{label.setText(tx,value)}catch(e){inner=true}})}"
              "catch(e){outer=true}if(!inner||!outer)throw Error('invalid text committed');}"
              "var mutations=[tx=>label.setReveal(tx,3),tx=>label.setReveal(tx,1.5),"
              "tx=>label.setText(tx,'x'.repeat(33))];for(var mutate of mutations){"
              "let failed=false;try{kasane.patch(mutate)}catch(e){failed=true}if(!failed)throw Error('limit');}"
              "var specs=[{text:'A',capacity:0},{text:'A',capacity:129},{text:'あ',capacity:2},"
              "{text:'A',font:'body\\0'},{text:'A',get capacity(){throw Error('capacity getter')}},"
              "{get text(){throw Error('text getter')}}];for(var spec of specs){let a=false,b=false;"
              "spec.bounds=[0,0,50,20];spec.color=0xffffffff;"
              "try{kasane.replace(tx=>{tx.background(255);try{tx.text(spec)}catch(e){a=true}})}catch(e){b=true}"
              "if(!a||!b)throw Error('bad spec');}"),
          "lone surrogates, controls, byte capacity, bad fonts/reveal and caught getters abort atomically");
    pocket_kasane_invalidate();check(present(&stats)==KSN_OK&&!memcmp(committed_pixels,panel_pixels,sizeof(panel_pixels)),
          "failed text changes preserve committed pixels");
    check(run("globalThis.oldLabel=label;kasane.replace(tx=>{tx.background(255);"
              "globalThis.label=tx.text({bounds:[0,7,240,23],text:'',capacity:128,color:0xffffffff})});"),
          "empty text reserves explicit update capacity");
    check(present(&stats)==KSN_OK,"empty text presents");
    check(run("let failed=false;try{kasane.patch(tx=>oldLabel.setText(tx,'A'))}catch(e){failed=e.code==='CLOSED'}"
              "if(!failed)throw Error('stale');globalThis.sequence=0;"),"stale text reference rejected");
    size_t live=live_allocations;uint32_t native=ksn_runtime_reserved_bytes();
    for(unsigned i=0;i<300;i++){
        if(!run("kasane.patch(tx=>{label.setText(tx,(++sequence&1)?'日本語':'A😀');label.setReveal(tx,1)});")||
           present(&stats)!=KSN_OK){check(false,"repeated text PATCH");break;}
    }
    JS_RunGC(rt);
    check(ksn_runtime_reserved_bytes()==native&&live_allocations<=live+2,"300 text updates keep native reservation and JS allocations bounded");
    close_fault_runtime();check(ksn_runtime_shutdown()==KSN_OK&&live_allocations==0,"text teardown frees guest and native owner");
}

static uint16_t image_over_black(uint16_t rgb,uint8_t alpha){
    unsigned r=rgb>>11,g=(rgb>>5)&63,b=rgb&31;
    r=((r*8+r/4)*alpha+127)/255;
    g=((g*4+g/16)*alpha+127)/255;
    b=((b*8+b/4)*alpha+127)/255;
    return (uint16_t)((r/8)*2048+(g/4)*32+b/8);
}
static void image_tests(void){
    check(open_fault_runtime(""),"image fixture opens");
    ksn_render_stats stats;ksn_image_port port;
    check(ksn_pet_builtin_image(&port)==KSN_OK,"real embedded PPT2 provider validates");
    check(run("globalThis.asset=kasane.petImage();globalThis.sprite=null;"
              "if(!kasane.features().image||asset.width!==64||asset.frames!==6||asset.variants!==12)throw Error('metadata');"
              "for(let i=0;i<100;i++)kasane.petImage();"
              "kasane.replace(tx=>{tx.background(255);sprite=tx.image({resource:asset,bounds:[0,0,64,64],clip:[0,0,240,135]})});"),
          "JS exposes a borrowed image and repeated handles");
    check(present(&stats)==KSN_OK,"JS image presents");
    bool pixels=true;uint16_t rgb[64];uint8_t alpha[64];
    for(unsigned y=0;y<64;y++){
        port.read_span(port.ctx,0,0,y,0,64,rgb,alpha);
        for(unsigned x=0;x<64;x++){
            if(panel_pixels[y*240+x]!=image_over_black(rgb[x],alpha[x])){
                if(pixels)printf("    pixel %u,%u actual=%04x expected=%04x rgb=%04x alpha=%u\n",x,y,
                    panel_pixels[y*240+x],image_over_black(rgb[x],alpha[x]),rgb[x],alpha[x]);
                pixels=false;
            }
        }
    }
    check(pixels,"JS image pixels match real PPT2 span");
    check(run("kasane.patch(tx=>sprite.setImageFrame(tx,11,5));"),"JS frame PATCH submitted");
    fail_band=1;check(present(&stats)==KSN_IO,"image partial transfer retains snapshot");fail_band=-1;
    check(run("for(let i=0;i<20;i++)kasane.petImage();"),"borrowing existing resource while pending does not mutate source");
    check(present(&stats)==KSN_OK,"image repair presents fixed variant and frame");
    pixels=true;
    for(unsigned y=0;y<64;y++){
        port.read_span(port.ctx,11,5,y,0,64,rgb,alpha);
        for(unsigned x=0;x<64;x++)if(panel_pixels[y*240+x]!=image_over_black(rgb[x],alpha[x]))pixels=false;
    }
    check(pixels,"repaired image uses submitted mood");
    for(unsigned size=1;size<=135;size+=7){
        char js[160];
        /* Signed formatting is important for the one-pixel destination. */
        snprintf(js,sizeof(js),"kasane.patch(tx=>sprite.setRect(tx,[-3,7,%d,%d]));",(int)size-3,(int)size+7);
        check(run(js)&&present(&stats)==KSN_OK,"JS setRect stretches the same source without REPLACE");
        pixels=true;
        for(unsigned y=0;y<135;y++)for(unsigned x=0;x<240;x++){
            uint16_t want=0;
            if((int)x<(int)size-3&&y>=7&&y<size+7){
                unsigned sx=(unsigned)((2ull*(x+3)+1)*64/(2*size));
                unsigned sy=(unsigned)((2ull*(y-7)+1)*64/(2*size));
                port.read_span(port.ctx,11,5,sy,sx,1,rgb,alpha);want=image_over_black(rgb[0],alpha[0]);
            }
            if(panel_pixels[y*240+x]!=want)pixels=false;
        }
        check(pixels,"stretched image and old footprint match pixel-center reference");
    }
    check(run("for(const bad of [{variant:12},{frame:6},{sourceX:33,scale:0.5},{scale:3},{sourceY:-1},{resource:{}},"
              "{get sourceX(){throw Error('getter')}}]){let failed=false;try{kasane.replace(tx=>{"
              "try{tx.image(Object.assign({resource:asset,bounds:[0,0,32,32]},bad))}catch(e){};"
              "tx.rect(shape)})}catch(e){failed=true}if(!failed)throw Error('accepted bad image')}"
              "let failed=false;try{kasane.patch(tx=>sprite.setImageFrame(tx,0,6))}catch(e){failed=true}"
              "if(!failed)throw Error('bad mood');"),"image validation and caught getter errors abort whole update");
    ksn_view *system;ksn_resource resources[15];
    check(ksn_runtime_system_acquire(&system)==KSN_OK,"SYSTEM image owner acquired");
    bool quota=true;for(unsigned i=0;i<15;i++)
        if(ksn_view_host_register_image(system,&port,&resources[i])!=KSN_OK)quota=false;
    ksn_resource extra;
    check(quota&&ksn_view_host_register_image(system,&port,&extra)==KSN_LIMIT,
          "120 JS handles consume exactly one of 16 native resources");
    pocket_kasane_reset();
    check(run("failed=false;try{kasane.replace(tx=>tx.image({resource:asset,bounds:[0,0,64,64]}))}"
              "catch(e){failed=e.code==='CLOSED'}if(!failed)throw Error('stale asset revived');"
              "asset=kasane.petImage();"),"APP reset invalidates old resource and reclaims its slot");
    ksn_draw draw={.kind=KSN_IMAGE,.bounds={0,0,32,32},.clip={0,0,240,135},.opacity=255,
        .data.image={.resource=resources[14],.variant=4,.frame=3,.scale=KSN_IMAGE_HALF}};
    ksn_tx tx;ksn_ref ref;
    check(ksn_view_begin(system,KSN_REPLACE,&tx)==KSN_OK&&ksn_view_add(system,tx,&draw,&ref)==KSN_OK&&
          ksn_view_submit(system,tx)==KSN_OK&&present(&stats)==KSN_OK,"SYSTEM images survive APP reset");
    close_fault_runtime();check(ksn_runtime_shutdown()==KSN_OK&&live_allocations==0,"image owners and guest teardown release all storage");
}

int main(void) {
    rt=JS_NewRuntime();ctx=JS_NewContext(rt);host_capabilities_clear();
    check(pocket_kasane_install(ctx,NULL)==ESP_OK,"namespace installs");
    check(run("if(kasane.features().capacity.refs!==32)throw Error('features');"
              "if(kasane.stats().active||kasane.stats().nativeBytes!==0)throw Error('lazy')"),
          "features do not allocate the native arena");
    check(!pocket_kasane_active(),"native display remains inactive after feature test");

    check(run("globalThis.tpl=kasane.cache.create(["
              "{bounds:[0,0,10,10],color:0xff0000ff},"
              "{bounds:[2,2,8,8],color:0x00ff00aa,opacity:180}]);"
              "globalThis.oldRef=null;globalThis.inst=null;"
              "globalThis.ticket=kasane.replace(tx=>{tx.background(0x000010ff);"
              "oldRef=tx.rect({bounds:[3,3,20,20],color:0xffffffff,opacity:200});"
              "let second=tx.rect({bounds:[8,8,25,25],color:0x2080ffff});"
              "tx.group(oldRef,2,190);inst=tx.instantiate(tpl,{offset:[30,20]});});"),
          "replace builds rect, group, and cached instance");
    check(pocket_kasane_active()&&pocket_kasane_has_submission(),
          "first submit claims display ownership");
    ksn_render_stats stats;transfers=0;
    check(present(&stats)==KSN_OK&&transfers==17&&stats.transferred_bytes==240*135*2,
          "initial replace presents all display bands");
    check(run("if(kasane.poll().status!=='PRESENTED')throw Error('poll')"),
          "poll reports presented state");

    check(run("ticket=kasane.patch(tx=>{oldRef.setRect(tx,[4,12,24,28]);"
              "oldRef.setColor(tx,0xf08020ff);inst.place(tx,{offset:[38,24],opacity:170});});"),
          "patch mutates refs without rebuilding topology");
    transfers=0;
    check(present(&stats)==KSN_OK&&transfers>0&&transfers<17,
          "patch transfers only dirty bands");

    check(run("globalThis.modalRef=null;ticket=kasane.replace(tx=>{"
              "tx.background(0x001020ff);tx.rect({bounds:[0,0,240,135],color:0x103050ff});"
              "tx.modal.open({backdrop:'dim-live',color:0x00000088,focus:7});"
              "modalRef=tx.rect({bounds:[40,25,200,110],color:0x80c0eedd,opacity:230});});"),
          "dim-live modal builds in a replace");
    check(present(&stats)==KSN_OK&&pocket_kasane_input_scope(false)==KSN_INPUT_MODAL,
          "modal input scope commits with presentation");
    check(run("let stale=false;try{kasane.patch(tx=>oldRef.setColor(tx,1))}"
              "catch(e){stale=e.code==='CLOSED'}if(!stale)throw Error('old ref live')"),
          "successful replace invalidates old draw refs");

    check(run("ticket=kasane.replace(tx=>{tx.background(0x101820ff);"
              "globalThis.newRef=tx.rect({bounds:[2,2,20,20],color:0xffffffff});"
              "tx.modal.close();});"),"modal close rebuilds app state");
    check(present(&stats)==KSN_OK&&pocket_kasane_input_scope(false)==KSN_INPUT_APP,
          "modal returns input to app after presentation");

    check(run("ticket=kasane.patch(tx=>newRef.setColor(tx,0x00ffffff));kasane.cancel(ticket);"
              "if(kasane.poll().status!=='DISCARDED')throw Error('cancel')"),
          "explicit cancel preserves the displayed state");
    check(!pocket_kasane_has_submission(),"cancel leaves no pending submission");

    check(run("ticket=kasane.patch(tx=>newRef.setRect(tx,[5,5,22,22]))"),
          "retry test submits a patch");
    fail_once=true;transfers=0;
    check(present(&stats)==KSN_IO&&pocket_kasane_has_submission()&&
          pocket_kasane_input_scope(false)==KSN_INPUT_BLOCKED,
          "LCD failure retains work and blocks app input");
    check(present(&stats)==KSN_OK&&!pocket_kasane_has_submission()&&
          pocket_kasane_input_scope(false)==KSN_INPUT_APP,
          "full repair retry commits and restores input");

    check(run("let rejected=false;try{kasane.patch(tx=>Promise.resolve())}"
              "catch(e){rejected=e.code==='INVALID_ARGUMENT'}"
              "if(!rejected)throw Error('thenable accepted')"),
          "thenable builders are rejected and aborted");
    check(!pocket_kasane_has_submission(),"thenable leaves no builder or submission");

    check(run("let limited=false;try{kasane.replace(tx=>{tx.background(0x000000ff);"
              "globalThis.refs=[];for(let i=0;i<33;i++)refs.push(tx.rect({"
              "bounds:[i,0,i+1,1],color:0xffffffff}));});}"
              "catch(e){limited=e.code==='LIMIT_EXCEEDED'}"
              "if(!limited)throw Error('ref limit')"),
          "draw-reference exposure is capped at 32");
    check(!pocket_kasane_has_submission(),"limit failure aborts the whole replace");

    check(run("if(kasane.stats().nativeBytes<=kasane.stats().cache.reservedBytes)throw Error('native accounting')"),
          "stats reports the allocated native arena");
    atomicity_tests();
    repair_tests();
    pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
    allocator_tests();
    base_block_tests();
    lazy_cache_tests();
    system_lifetime_tests();
    primitive_tests();
    text_tests();
    image_tests();
    printf("%s: %u failure(s)\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
