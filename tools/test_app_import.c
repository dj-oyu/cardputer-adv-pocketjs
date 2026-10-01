// Static import on the host: the REAL pocket_app_load.c (its module loader and
// pocket_app_eval_module(), the call app_session.c's eval_reporting() makes for
// a .mjs entry), pocket_api.c and app_registry.c on the real QuickJS, with the
// chunk table read from apps/heapprobe/import_chunks.txt and appload_chunks.txt
// (the lists the device probe embeds) and the generated comparison program
// (tools/heapprobe_import_gen.py, written by build_app_import_test.sh).
//
// What it settles, per docs/vm/eval-peak.md section 9: an entry importing by
// chunk name, live bindings, one instance per module, a harmless cycle; every
// refusal as the pending exception eval_reporting() prints -- with the chunk's
// file:line where there is one -- for a syntax error, a throw, a TDZ cycle, an
// unknown name, a path, a script chunk, a module handed to load(), import
// attributes, a chain deeper than 8, top-level await, dynamic import() from a
// module and from a script, and no unhandled rejection in a session that works;
// out of memory while importing, then the same entry on a bigger heap; the
// state starting over each session; and the smallest heap limit that
// evaluates the same program as one script, as three load()ed scripts and as
// three imported modules (host 64-bit: a ratio, not the board's number).
//
//   wsl -e bash -lc "cd /mnt/c/.../cardputer-adv-pocketjs && bash tools/build_app_import_test.sh"
#include "pocket_api.h"
#include "pocket_app_load.h"
#include "app_registry.h"
#include "app_chunks_host.h"
#include "esp_app_desc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int64_t esp_timer_get_time(void) { return 1000000; }
static const esp_app_desc_t description={.version="host"};
const esp_app_desc_t *esp_app_get_description(void) { return &description; }

static JSRuntime *rt;
static JSContext *ctx;
static unsigned checks, unhandled;

#define REQUIRE(c) do { if(!(c)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); exit(1); } } while(0)

static void fail_with_exception(const char *what) {
    JSValue e=JS_GetException(ctx);
    const char *s=JS_ToCString(ctx,e);
    fprintf(stderr,"FAIL %s: %s\n",what,s?s:"?");
    exit(1);
}

static void run(const char *name, const char *script) {
    JSValue v=JS_Eval(ctx,script,strlen(script),name,JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(v)) fail_with_exception(name);
    JS_FreeValue(ctx,v);
    checks++;
}

static void tracker(JSContext *c, JSValueConst promise, JSValueConst reason, bool handled,
                    void *opaque) {
    (void)c; (void)promise; (void)reason; (void)opaque;
    if(!handled) unhandled++;
}

static void session_begin(const char *app_id) {
    rt=JS_NewRuntime();
    REQUIRE(rt);
    ctx=JS_NewContext(rt);
    REQUIRE(ctx);
    JS_SetHostPromiseRejectionTracker(rt,tracker,NULL);
    unhandled=0;
    app_registry_select(app_id);
    REQUIRE(pocket_api_install(ctx,NULL)==ESP_OK);
    REQUIRE(pocket_app_load_install(ctx)==ESP_OK);
    run("prelude.js","globalThis.check=(c,m)=>{if(!c)throw Error('check failed: '+m)};"
        "globalThis.console={log(){}};");
}

static void session_end(void) {
    JS_RunGC(rt);
    pocket_api_reset();
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    ctx=NULL; rt=NULL;
}

static void drain(void) {
    JSContext *c;
    while(JS_ExecutePendingJob(rt,&c)>0) {}
}

// The module entry, the way eval_reporting() evaluates it. Must succeed.
static void entry_ok(const char *source) {
    JSValue v=pocket_app_eval_module(ctx,source,strlen(source),"user.js");
    if(JS_IsException(v)) fail_with_exception(source);
    REQUIRE(JS_IsUndefined(v));
    checks++;
}

// Must fail; the pending exception's String() and first stack line -- what
// eval_reporting() prints on EVAL_ERROR -- must contain both needles.
static void entry_fails(const char *source, const char *needle, const char *where) {
    JSValue v=pocket_app_eval_module(ctx,source,strlen(source),"user.js");
    REQUIRE(JS_IsException(v));
    JSValue e=JS_GetException(ctx);
    char line[512]={0};
    const char *text=JS_ToCString(ctx,e);
    if(text) { snprintf(line,sizeof line,"%s",text); JS_FreeCString(ctx,text); }
    if(JS_IsObject(e)) {
        JSValue stack=JS_GetPropertyStr(ctx,e,"stack");
        const char *s=JS_IsString(stack)?JS_ToCString(ctx,stack):NULL;
        if(s) {
            const char *nl=strchr(s,'\n');
            size_t used=strlen(line);
            snprintf(line+used,sizeof line-used," %.*s",nl?(int)(nl-s):(int)strlen(s),s);
            JS_FreeCString(ctx,s);
        }
        JS_FreeValue(ctx,stack);
    }
    JS_FreeValue(ctx,e);
    if(!strstr(line,needle) || (where && !strstr(line,where))) {
        fprintf(stderr,"FAIL entry %s\n  got  %s\n  want %s / %s\n",source,line,needle,
                where?where:"-");
        exit(1);
    }
    printf("  EVAL_ERROR %s\n",line);
    checks++;
}

static char *slurp(const char *path) {
    FILE *f=fopen(path,"rb");
    REQUIRE(f);
    fseek(f,0,SEEK_END);
    long n=ftell(f);
    fseek(f,0,SEEK_SET);
    char *b=malloc((size_t)n+1);
    REQUIRE(b && fread(b,1,(size_t)n,f)==(size_t)n);
    b[n]=0;
    fclose(f);
    return b;
}

// Does `source` evaluate in a fresh session under `limit` bytes of guest heap?
static bool fits(const char *source, bool module, size_t limit) {
    session_begin("local.hello");
    JS_SetMemoryLimit(rt,limit);
    JSValue v=module?pocket_app_eval_module(ctx,source,strlen(source),"user.js")
                    :JS_Eval(ctx,source,strlen(source),"user.js",JS_EVAL_TYPE_GLOBAL);
    const bool ok=!JS_IsException(v);
    if(!ok) JS_FreeValue(ctx,JS_GetException(ctx));
    JS_FreeValue(ctx,v);
    JS_SetMemoryLimit(rt,0);
    session_end();
    return ok;
}

static size_t smallest(const char *source, bool module) {
    size_t lo=40000, hi=1200000;
    REQUIRE(fits(source,module,hi) && !fits(source,module,lo));
    while(hi-lo>256) {
        const size_t mid=(lo+hi)/2;
        if(fits(source,module,mid)) hi=mid; else lo=mid;
    }
    return hi;
}

int main(int argc, char **argv) {
    REQUIRE(argc==2);   // the generator's output directory
    const char *gen=argv[1];
    REQUIRE(app_chunks_host_read("apps/heapprobe/appload_chunks.txt")==0);
    REQUIRE(app_chunks_host_read("apps/heapprobe/import_chunks.txt")==0);
    char path[512];
    snprintf(path,sizeof path,"%s/imp_chunks.txt",gen);
    REQUIRE(app_chunks_host_read(path)==0);
    // A chain of 10 modules, for the depth refusal, under another identity so
    // local.hello's set stays within its 32.
    char dir[]="/tmp/app-import-XXXXXX";
    REQUIRE(mkdtemp(dir)!=NULL);
    char list[4096]="app local.stress\n";
    for(int i=0;i<10;i++) {
        snprintf(path,sizeof path,"%s/deep_%d.mjs",dir,i);
        FILE *f=fopen(path,"w");
        REQUIRE(f);
        if(i<9) fprintf(f,"import { d } from 'deep%d';\nexport const d%d = d;\nexport { d };\n",i+1,i);
        else fprintf(f,"export const d = 9;\n");
        fclose(f);
        size_t used=strlen(list);
        snprintf(list+used,sizeof list-used,"deep%d deep_%d.mjs\n",i,i);
    }
    snprintf(path,sizeof path,"%s/chunks.txt",dir);
    FILE *f=fopen(path,"w");
    REQUIRE(f);
    fputs(list,f);
    fclose(f);
    REQUIRE(app_chunks_host_read(path)==0);
    // The table knows the kind from the file name.
    const app_chunk_set_t *hello=app_chunks_for("local.hello");
    REQUIRE(hello);
    for(uint32_t k=0;k<hello->count;k++) {
        const char *file=hello->chunks[k].file;
        REQUIRE(hello->chunks[k].module==(strstr(file,".mjs")!=NULL));
    }

    for(int session=0;session<3;session++) {
        session_begin("local.hello");
        // The entry: named imports from two modules that share a third, and
        // frame() set on globalThis because a module's names are its own.
        entry_ok("import { count, bump, name } from 'imok';\n"
                 "import { base } from 'imbase';\n"
                 "import * as cyc from 'imcycb';\n"
                 "check(count === 40 && base === 40 && name === 'ok', 'values');\n"
                 "bump(); bump();\n"
                 "check(count === 42, 'a live binding, not a copy: ' + count);\n"
                 "check(globalThis.imBaseRuns === 1, 'imbase evaluated once: ' + imBaseRuns);\n"
                 "check(cyc.cb() === 'b' && cyc.cab() === 'ab', 'a harmless cycle');\n"
                 "check(typeof globalThis.count === 'undefined', 'module names are not global');\n"
                 "let threw = false; try { eval('count = 1'); } catch (e) { threw = e instanceof TypeError; }\n"
                 "check(threw && count === 42, 'an import is read-only');\n"
                 "globalThis.frame = () => count;\n");
        run("after.js","check(typeof frame==='function'&&frame()===42,'frame sees the module')");
        // A module chunk is not a script: load() refuses it without evaluating.
        run("load-module.js",
            "let e=null;try{pocket.app.load('imok')}catch(x){e=x}"
            "check(e&&e.code==='INVALID_ARGUMENT'&&/is a module/.test(e.message),String(e));"
            "check(pocket.app.load('ok')===true,'a script chunk still loads');");
        // import() after the evaluation, from a script: refused, as a rejection.
        run("dyn.js","globalThis.dynErr=null;import('imbase').then(()=>{dynErr='resolved'},e=>{dynErr=String(e)});");
        drain();
        run("dyn-check.js","check(/dynamic import\\(\\) is not supported/.test(dynErr),dynErr)");
        REQUIRE(unhandled==0);
        session_end();
    }

    // Each failure in its own session: a failed entry ends the session in the
    // firmware (START_FAILED), so nothing is carried from one to the next.
    struct { const char *source, *needle, *where; } fails[]={
        {"import { a } from 'imbad';", "SyntaxError", "import_bad.mjs:3"},
        {"import { before } from 'imthrow';", "TypeError", "import_throw.mjs:4"},
        {"import { ta } from 'imtdza';", "ReferenceError: ta is not initialized", "import_tdz_b.mjs:1"},   // QuickJS points at the import
        {"import { x } from 'nope';", "user.js imports 'nope': this app has no chunk 'nope'", NULL},
        {"import { x } from './import_ok.mjs';", "no chunk './import_ok.mjs'", NULL},
        {"import { x } from '/abs/x.mjs';", "no chunk '/abs/x.mjs'", NULL},
        {"import { x } from 'ok';", "that chunk is a script (appload_ok.js)", NULL},
        {"import { count } from 'imok' with { type: 'json' };", "import attributes", NULL},
        {"import { d } from 'deep0';", "RangeError: import of deep_8.mjs: imports nest deeper than 8", NULL},
        {"import { x } from 'imtla';", "top-level await is not supported", NULL},
        {"export const y = 1;\nawait 0;", "top-level await is not supported", NULL},
        {"const = 1;", "SyntaxError", "user.js:1"},
        {"null.x;", "TypeError", "user.js:1"},
    };
    for(size_t i=0;i<sizeof fails/sizeof fails[0];i++) {
        session_begin(strstr(fails[i].source,"deep")?"local.stress":"local.hello");
        entry_fails(fails[i].source,fails[i].needle,fails[i].where);
        session_end();
    }

    // A depth of 8 is allowed: deep2 .. deep9.
    {
        session_begin("local.stress");
        entry_ok("import { d, d2 } from 'deep2';\ncheck(d === 9 && d2 === 9, 'chain');\n");
        session_end();
    }

    // import() at a module's top level: the module evaluates, the promise rejects.
    {
        session_begin("local.hello");
        entry_ok("import { p } from 'imdyn';\nglobalThis.dynP = p;\n");
        run("dyn2.js","globalThis.dynErr=null;dynP.then(()=>{dynErr='resolved'},e=>{dynErr=String(e)});");
        drain();
        run("dyn2-check.js","check(/import\\('imbase'\\) in import_dyn\\.mjs: dynamic import\\(\\) is not supported/.test(dynErr),dynErr)");
        session_end();
    }

    // Out of memory while a chunk compiles: an exception, no crash; the same
    // entry evaluates in a fresh session with room.
    snprintf(path,sizeof path,"%s/imp_entry_import.mjs",gen);
    char *imp=slurp(path);
    {
        session_begin("local.hello");
        JS_SetMemoryLimit(rt,120000);
        JSValue v=pocket_app_eval_module(ctx,imp,strlen(imp),"user.js");
        REQUIRE(JS_IsException(v));
        JSValue e=JS_GetException(ctx);
        const char *text=JS_ToCString(ctx,e);
        REQUIRE(JS_IsNull(e) || (text && strstr(text,"out of memory")));
        printf("  OOM while importing: %s\n",text?text:"null");
        if(text) JS_FreeCString(ctx,text);
        JS_FreeValue(ctx,e);
        JS_SetMemoryLimit(rt,0);
        session_end();
        session_begin("local.hello");
        entry_ok(imp);
        session_end();
        checks++;
    }

    // The same program three ways: the smallest limit each evaluates under.
    snprintf(path,sizeof path,"%s/imp_all.js",gen);
    char *all=slurp(path);
    snprintf(path,sizeof path,"%s/imp_entry_load.js",gen);
    char *load=slurp(path);
    const size_t a=smallest(all,false), b=smallest(load,false), c=smallest(imp,true);
    printf("  smallest limit (host 64-bit, 256 B steps): one script %zu, load() x3 %zu (-%zu), "
           "import x3 %zu (-%zu)\n",a,b,a-b,c,a-c);
    // What stays after the evaluation (GC'd), which is where modules pay.
    size_t held[3];
    const char *srcs[3]={all,load,imp};
    for(int i=0;i<3;i++) {
        session_begin("local.hello");
        JSValue v=i==2?pocket_app_eval_module(ctx,srcs[i],strlen(srcs[i]),"user.js")
                      :JS_Eval(ctx,srcs[i],strlen(srcs[i]),"user.js",JS_EVAL_TYPE_GLOBAL);
        REQUIRE(!JS_IsException(v));
        JS_FreeValue(ctx,v);
        JS_RunGC(rt);
        size_t u=0,l=0;
        JS_GetMemoryCounters(rt,&u,&l);
        held[i]=u;
        session_end();
    }
    printf("  held after evaluation: one script %zu, load() x3 %zu, import x3 %zu\n",
           held[0],held[1],held[2]);
    REQUIRE(c<a && b<a);
    free(all); free(load); free(imp);

    app_chunks_host_clear();
    printf("APP_IMPORT_OK: %u checks: imports by chunk name, live bindings, one instance, "
           "harmless cycle, syntax/throw/TDZ/unknown/path/script/attributes/depth/TLA/dynamic "
           "refusals with file:line, no stray unhandled rejection, OOM then retry, "
           "3 sessions, peak comparison\n",checks);
    return 0;
}
