// pocket.app.load() on the host: the REAL pocket_app_load.c, pocket_api.c and
// app_registry.c on the real QuickJS, with the chunk table read from
// apps/heapprobe/appload_chunks.txt (the list the device check embeds) through
// tools/hostshim/app_chunks_host.c, plus a few chunks this test writes itself.
//
// What it settles, per docs/vm/eval-peak.md section 7: the capability and its
// limits without building the namespace; loads from the evaluation's top
// level and from a later call (frame()); idempotence; every refusal and its
// PocketError (code, outcome, retryable, cause, the chunk's file:line in the
// message); global let/const/var/function shared across scripts; a failed
// compile retried, a failed run refused; a heap too small, then big enough;
// the watchdog's uncatchable error passing through untouched; an app with no
// chunks; the state starting over each session; and what dropping a chunk's
// only reference gives back to the heap.
//
//   wsl -e bash -lc "cd /mnt/c/.../cardputer-adv-pocketjs && bash tools/build_app_load_test.sh"
#include "pocket_api.h"
#include "pocket_app_load.h"
#include "app_registry.h"
#include "app_chunks_host.h"
#include "esp_app_desc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

int64_t esp_timer_get_time(void) { return 1000000; }
static const esp_app_desc_t description={.version="host"};
const esp_app_desc_t *esp_app_get_description(void) { return &description; }

static JSRuntime *rt;
static JSContext *ctx;
static unsigned checks;

#define REQUIRE(c) do { if(!(c)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); exit(1); } } while(0)

static void fail_with_exception(const char *what) {
    JSValue e=JS_GetException(ctx);
    const char *s=JS_ToCString(ctx,e);
    fprintf(stderr,"FAIL %s: %s\n",what,s?s:"?");
    exit(1);
}

// Evaluates a script that must not throw. `name` is the file its stack shows.
static void run(const char *name, const char *script) {
    JSValue v=JS_Eval(ctx,script,strlen(script),name,JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(v)) fail_with_exception(name);
    JS_FreeValue(ctx,v);
    checks++;
}

// Calls a global function the way the host turn calls frame(). Returns the
// result, or JS_EXCEPTION with the exception still pending.
static JSValue call(const char *name) {
    JSValue g=JS_GetGlobalObject(ctx);
    JSValue f=JS_GetPropertyStr(ctx,g,name);
    JSValue r=JS_Call(ctx,f,JS_UNDEFINED,0,NULL);
    JS_FreeValue(ctx,f);
    JS_FreeValue(ctx,g);
    return r;
}

// The shared assertion helper and a PocketError checker, in the realm.
static const char PRELUDE[]=
    "globalThis.check=(c,m)=>{if(!c)throw Error('check failed: '+m)};"
    "globalThis.err=f=>{try{f();return null}catch(e){return e}};"
    "globalThis.pe=(e,code,outcome,retry)=>{"
    "check(e&&e.code===code,'code '+(e&&e.code)+' want '+code+' '+(e&&e.message));"
    "check(e.operation==='app.load','operation '+e.operation);check(e instanceof Error,'is an Error');"
    "if(outcome!==undefined)check(e.outcome===outcome,'outcome '+e.outcome+' want '+outcome);"
    "if(retry!==undefined)check(e.retryable===retry,'retryable '+e.retryable);return e};";

static void session_begin(const char *app_id) {
    rt=JS_NewRuntime();
    REQUIRE(rt);
    ctx=JS_NewContext(rt);
    REQUIRE(ctx);
    app_registry_select(app_id);
    REQUIRE(pocket_api_install(ctx,NULL)==ESP_OK);
    // pocket_app_install() calls this in the firmware; the rest of pocket.app
    // is pocket_app.c's and does not enter into what is tested here.
    REQUIRE(pocket_app_load_install(ctx)==ESP_OK);
    run("prelude.js",PRELUDE);
}

static void session_end(void) {
    JS_RunGC(rt);
    pocket_api_reset();
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    ctx=NULL; rt=NULL;
}

static size_t used(void) {
    size_t u=0,l=0;
    JS_RunGC(rt);
    JS_GetMemoryCounters(rt,&u,&l);
    return u;
}

static int spin_interrupt(JSRuntime *r, void *opaque) { (void)r; return *(int *)opaque; }

static void write_file(const char *path, const char *text) {
    FILE *f=fopen(path,"w");
    REQUIRE(f);
    fputs(text,f);
    fclose(f);
}

int main(void) {
    // The device list, and a list of this test's own for the same app:
    // merging two lists into one set is part of what the shim promises.
    REQUIRE(app_chunks_host_read("apps/heapprobe/appload_chunks.txt")==0);
    char dir[]="/tmp/app-load-XXXXXX";
    REQUIRE(mkdtemp(dir)!=NULL);
    char path[256], list[256];
    snprintf(path,sizeof path,"%s/appload_uses.js",dir);
    write_file(path,"// Reads the first chunk's lexical names at its own top level.\n"
                    "globalThis.alUses = alC.n + alS;\n");
    snprintf(path,sizeof path,"%s/appload_spin.js",dir);
    write_file(path,"globalThis.alSpun = 0;\nfor (;;) alSpun++;\n");
    snprintf(path,sizeof path,"%s/appload_frame.js",dir);
    write_file(path,"// Defines what the entry's frame() calls.\nvar alFrame = n => n + 1;\n");
    snprintf(path,sizeof path,"%s/appload_dup.js",dir);
    write_file(path,"// Declares a name the first chunk has.\nlet alS = 5;\n");
    snprintf(list,sizeof list,"%s/chunks.txt",dir);
    write_file(list,"app local.hello\nuses appload_uses.js\nspin appload_spin.js\n"
                    "frame appload_frame.js # a comment\ndup appload_dup.js\n");
    REQUIRE(app_chunks_host_read(list)==0);
    // The shim refuses what the build refuses.
    snprintf(path,sizeof path,"%s/bad.txt",dir);
    write_file(path,"app local.hello\nok appload_frame.js\n");
    REQUIRE(app_chunks_host_read(path)!=0);   // 'ok' is local.hello's already
    write_file(path,"ok appload_frame.js\n");
    REQUIRE(app_chunks_host_read(path)!=0);   // no app line
    write_file(path,"app local.x\nthis/name appload_frame.js\n");
    REQUIRE(app_chunks_host_read(path)!=0);   // not a chunk name
    write_file(path,"app local.x\nnofile nothing.js\n");
    REQUIRE(app_chunks_host_read(path)!=0);   // a missing file

    // Three sessions: the loaded/failed state is the session's, so the same
    // loads succeed again in a fresh realm.
    for(int session=0;session<3;session++) {
        session_begin("local.hello");
        // Section 2: the capability is readable without building pocket.app.
        run("cap.js",
            "check(typeof Object.getOwnPropertyDescriptor(pocket,'app').get==='function','app is lazy');"
            "const c=pocket.capabilities.get('app.load');"
            "check(c.supported&&c.available,'supported');"
            "check(c.limits.maxChunks===32&&c.limits.maxNameBytes===31,'limits '+JSON.stringify(c.limits));"
            "check(typeof Object.getOwnPropertyDescriptor(pocket,'app').get==='function','still lazy');"
            "check(typeof pocket.app.load==='function','load');"
            "check(Object.getOwnPropertyDescriptor(pocket,'app').get===undefined,'built on read');");
        // The evaluation's top level: the shape a split app's entry has.
        run("entry.js",
            "const L=pocket.app.load;"
            "check(L('ok')===true,'first load');check(L('ok')===false,'idempotent');"
            "check(alS===10&&alC.n===2&&alV===3&&alG===4&&alF()===12,'shared names');"
            "alS=20;check(alF()===22,'one binding, not a copy');"
            "check(L('uses')===true&&alUses===22,'a chunk reads the last one\\'s let/const');"
            "check(L('self')===true&&alSelf==='CONFLICT','load cycle');"
            "check(L('frame')===true&&alFrame(1)===2,'frame chunk');"
            "globalThis.frame=()=>{check(L('mid')===true,'mid in frame');check(L('mid')===false,'again');"
            "return alMid.sum()};");
        // frame(), called from C the way the host turn calls it.
        JSValue r=call("frame");
        if(JS_IsException(r)) fail_with_exception("frame()");
        int32_t sum=0;
        REQUIRE(!JS_ToInt32(ctx,&sum,r) && sum>0);
        JS_FreeValue(ctx,r);
        checks++;
        // Refusals, each a PocketError with a section 4 code.
        run("refuse.js",
            "const R=pocket.app.load;"
            "pe(err(()=>R('nope')),'NOT_FOUND','not-applied',false);"
            "pe(err(()=>R('y'.repeat(31))),'NOT_FOUND','not-applied',false);"
            "for(const a of [undefined,3,'','x'.repeat(32),'a\\0b',{toString(){return 'ok'}}])"
            "  pe(err(()=>R(a)),'INVALID_ARGUMENT','not-applied',false);"
            "pe(err(()=>R()),'INVALID_ARGUMENT');"
            // A compile that fails: nothing declared, may be asked again.
            "let e=pe(err(()=>R('bad')),'CORRUPT_DATA','not-applied',false);"
            "check(e.cause instanceof SyntaxError,'cause '+e.cause);"
            "check(/did not compile: SyntaxError.*appload_bad\\.js:3/.test(e.message),e.message);"
            "check(typeof alB==='undefined','nothing declared');"
            "pe(err(()=>R('bad')),'CORRUPT_DATA','not-applied',false);"
            // A run that throws: half declared, never run again.
            "e=pe(err(()=>R('thr')),'CORRUPT_DATA','unknown',false);"
            "check(e.cause instanceof TypeError,'cause '+e.cause);"
            "check(/threw while it ran: TypeError.*appload_throw\\.js:4/.test(e.message),e.message);"
            "check(alTf()===1,'what ran before the throw is there');"
            "e=pe(err(()=>R('thr')),'CORRUPT_DATA','not-applied',false);"
            "check(/not run again/.test(e.message),e.message);"
            // Two chunks declaring one `let`: refused when the second starts
            // to run, before any of it has -- the first one's binding stays.
            "e=pe(err(()=>R('dup')),'CORRUPT_DATA','unknown',false);"
            "check(e.cause instanceof SyntaxError&&/redeclaration/.test(e.message),e.message);"
            "check(alS===20,'untouched');"
            "globalThis.uncaught=()=>R('bad');");
        // Uncaught, the PocketError reaches the host naming the chunk's line.
        r=call("uncaught");
        REQUIRE(JS_IsException(r));
        JSValue e=JS_GetException(ctx);
        const char *text=JS_ToCString(ctx,e);
        REQUIRE(text && strstr(text,"Error: chunk 'bad' did not compile: SyntaxError") &&
                strstr(text,"appload_bad.js:3"));
        if(session==0) printf("  uncaught: %s\n",text);
        JS_FreeCString(ctx,text);
        JS_FreeValue(ctx,e);
        checks++;
        session_end();
    }

    // What a chunk costs while it is referenced, and what dropping it gives
    // back: `mid` keeps its 48 methods behind one var.
    {
        session_begin("local.hello");
        run("warm.js","pocket.app.load('ok');var alMid;");
        const size_t before=used();
        run("load.js","pocket.app.load('mid');check(alMid.sum()>0,'runs')");
        const size_t with=used();
        run("drop.js","alMid=null;");
        const size_t dropped=used();
        printf("  mid (5,023 B of source): guest heap %zu -> %zu with it -> %zu after alMid=null "
               "(kept %zd, given back %zd)\n",before,with,dropped,
               (ssize_t)(with-before),(ssize_t)(with-dropped));
        REQUIRE(with>before && dropped<before+(with-before)/4);
        // Idempotent even after the drop: the chunk is not evaluated again.
        run("again.js","check(pocket.app.load('mid')===false&&alMid===null,'not reloaded')");
        session_end();
    }

    // A heap too small to compile `mid`: OUT_OF_MEMORY, retryable, nothing
    // declared -- and the same load succeeds once the heap is big enough.
    {
        session_begin("local.hello");
        run("warm.js","pocket.app.load('ok');var alMid;");
        const size_t base=used();
        JS_SetMemoryLimit(rt,base+6000);
        run("oom.js",
            "pe(err(()=>pocket.app.load('mid')),'OUT_OF_MEMORY','not-applied',true);"
            "check(alMid===undefined,'nothing declared');");
        JS_SetMemoryLimit(rt,0);
        run("retry.js","check(pocket.app.load('mid')===true&&alMid.sum()>0,'loaded on retry')");
        printf("  a limit of used+6000 B: OUT_OF_MEMORY, retryable; loaded once the limit went\n");
        session_end();
    }

    // The watchdog: its uncatchable error must pass through load(), not
    // become a PocketError a try/catch could swallow.
    {
        session_begin("local.hello");
        int stop=0;
        JS_SetInterruptHandler(rt,spin_interrupt,&stop);
        run("arm.js","globalThis.caught=0;globalThis.go=()=>{try{pocket.app.load('spin')}catch(e){caught++}};");
        stop=1;
        JSValue r=call("go");
        REQUIRE(JS_IsException(r));
        JSValue e=JS_GetException(ctx);
        REQUIRE(JS_IsUncatchableError(e));
        JS_FreeValue(ctx,e);
        stop=0;
        JS_SetInterruptHandler(rt,NULL,NULL);
        run("after.js","check(caught===0,'not caught');check(alSpun>0,'it ran');"
            "pe(err(()=>pocket.app.load('spin')),'CORRUPT_DATA','not-applied',false);");
        printf("  interrupted load: the uncatchable error passed through; the chunk is refused after\n");
        session_end();
    }

    // An app with no chunks -- the Playground receives its source at run
    // time -- answers NOT_FOUND for every name, and still supports app.load.
    {
        session_begin("local.playground");
        run("playground.js",
            "check(pocket.capabilities.get('app.load').supported,'supported');"
            "pe(err(()=>pocket.app.load('ok')),'NOT_FOUND','not-applied',false);");
        session_end();
    }

    app_chunks_host_clear();
    printf("APP_LOAD_OK: %u checks: capability and lazy namespace, top-level and frame() loads, "
           "idempotence, shared let/const/var/function, NOT_FOUND / INVALID_ARGUMENT / "
           "CORRUPT_DATA / CONFLICT / OUT_OF_MEMORY, watchdog passthrough, no-chunk app, "
           "3 sessions, release\n",checks);
    return 0;
}
