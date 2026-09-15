// End-to-end host contract for pocket.kasane with the real QuickJS and the
// real fixed-storage DS core/cache/modal/renderer.
#include "pocket_kasane.h"
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

void *__real_calloc(size_t count,size_t size);
void *__wrap_calloc(size_t count,size_t size) {
    if(native_fault) { native_fault=false;return NULL; }
    return __real_calloc(count,size);
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
                          .width=240,.height=135,.strip_rows=8};
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
            if(!JS_IsException(result)||!JS_HasException(ctx)||pocket_kasane_has_submission())
                passed=false;
            JSValue error=JS_GetException(ctx);JS_FreeValue(ctx,error);
            if(!run("let s=kasane.stats();if(s.displayed.commands!==2||s.cache.instances!==1||"
                    "s.cache.templates!==1||s.cache.commands!==1)throw Error('fault quota');"
                    "kasane.patch(tx=>baseRef.setColor(tx,0x00ff00ff));"
                    "kasane.cancel(kasane.poll().ticket);")) passed=false;
        } else {
            if(JS_IsException(result)||JS_HasException(ctx)) passed=false;
            finished=true;
        }
        JS_FreeValue(ctx,result);JS_FreeValue(ctx,function);close_fault_runtime();
        if(live_allocations) passed=false;
        if(finished||!passed) break;
    }
    printf("    %s: %u allocation failures injected\n",label,injected);
    check(passed&&finished&&injected>0,label);
}

static void allocator_tests(void) {
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

    check(run("if(kasane.stats().nativeBytes<13000)throw Error('native accounting')"),
          "stats reports the allocated native arena");
    atomicity_tests();
    repair_tests();
    pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
    allocator_tests();
    printf("%s: %u failure(s)\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
