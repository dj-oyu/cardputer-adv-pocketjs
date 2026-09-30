#include "pocket_app_load.h"
#include "pocket_api.h"
#include "app_chunks.h"
#include "app_registry.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "pocket.app";

// The set is fixed at install: app_registry_current() is the identity the
// session started under (main.c selects it before app_start_source(), the
// overlay host before its own start, app_resume() again on a wake), and a
// realm keeps its identity for its whole life. Reading it per call would let
// a realm that outlived a selection -- a suspended app -- reach another app's
// chunks.
static const app_chunk_set_t *set;

// One bit a chunk (APP_CHUNKS_MAX is 32). `loaded` answers the second load()
// without evaluating anything: a global script that declared a `let` or a
// `const` cannot be evaluated into the same realm again -- the second pass is
// a SyntaxError for the redeclaration, naming a line the app never got wrong.
// `failed` is a chunk whose top level started and threw: some of its global
// bindings may exist already (a `let` stays in its dead zone for good), so it
// is refused rather than half-run twice. `loading` is a chunk on the C stack
// right now, so a chunk that loads itself, or two that load each other, meet
// a CONFLICT instead of a load that silently returns early with the other
// half's declarations missing.
static uint32_t loaded, failed, loading;

// Bytes of the error line. The PocketError keeps the original exception as
// `cause`, stack and all, so this is only what String(e) shows -- the line
// the Playground's error screen and FRAME_WRAP's __pjs_error print.
#define LOAD_MESSAGE_MAX 192

static int find(const char *name, size_t length) {
    if(!set) return -1;
    for(uint32_t i=0;i<set->count;i++)
        if(strlen(set->chunks[i].name)==length && !memcmp(set->chunks[i].name,name,length))
            return (int)i;
    return -1;
}

// QuickJS reports running out of heap as InternalError("out of memory"), or,
// once even that error cannot be allocated, as a bare null (JS_TakeOOMCanary's
// comment in quickjs.h). The canary itself is the host's to take once per
// turn -- report_oom_if_any() in app_session.c logs it -- so taking it here
// would steal that line; the exception is read instead. A chunk that throws
// null itself is misfiled as OUT_OF_MEMORY, which is the cost of not reaching
// into the engine for a second counter.
static bool message_is(JSContext *ctx, JSValueConst exception, const char *text) {
    if(!JS_IsError(exception)) return false;
    JSValue message=JS_GetPropertyStr(ctx,exception,"message");
    if(JS_IsException(message)) { JS_FreeValue(ctx,JS_GetException(ctx)); return false; }
    const char *s=JS_IsString(message)?JS_ToCString(ctx,message):NULL;
    bool same=s && !strcmp(s,text);
    if(s) JS_FreeCString(ctx,s);
    else if(JS_IsString(message)) JS_FreeValue(ctx,JS_GetException(ctx));
    JS_FreeValue(ctx,message);
    return same;
}

// "SyntaxError: unexpected token ... at scene.js:12:5": String(e) and the
// first line of its stack, the same two things eval_reporting() puts on the
// EVAL_ERROR line, so a failure inside a chunk names the chunk's file and line
// rather than the load() call that reached it.
static void describe(JSContext *ctx, JSValueConst exception, char *out, size_t size) {
    size_t used=strlen(out);
    const char *text=JS_ToCString(ctx,exception);
    if(!text) { JS_FreeValue(ctx,JS_GetException(ctx)); return; }
    snprintf(out+used,size-used,"%s",text);
    JS_FreeCString(ctx,text);
    if(!JS_IsObject(exception)) return;
    JSValue stack=JS_GetPropertyStr(ctx,exception,"stack");
    if(JS_IsException(stack)) { JS_FreeValue(ctx,JS_GetException(ctx)); return; }
    const char *s=JS_IsString(stack)?JS_ToCString(ctx,stack):NULL;
    if(s) {
        const char *p=s;
        while(*p==' ') p++;
        const char *nl=strchr(p,'\n');
        int n=nl?(int)(nl-p):(int)strlen(p);
        used=strlen(out);
        if(n>0) snprintf(out+used,size-used," %.*s",n,p);
        JS_FreeCString(ctx,s);
    }
    JS_FreeValue(ctx,stack);
}

// Turns the pending exception of a failed compile (`ran` false) or a failed
// top-level run (`ran` true) into the PocketError load() throws.
static JSValue fail(JSContext *ctx, const app_chunk_t *chunk, bool ran) {
    static const char *const OP="app.load";
    JSValue exception=JS_GetException(ctx);
    // The session's watchdog (the 2 s evaluation deadline, a turn's 250 ms, a
    // stop request) ends a run with an uncatchable error. Converting it into
    // a PocketError would make it catchable and let the app carry on past the
    // deadline that was meant to end it, so it goes on exactly as it came.
    if(JS_IsUncatchableError(exception)) return JS_Throw(ctx,exception);

    const bool oom=JS_IsNull(exception) || message_is(ctx,exception,"out of memory");
    const bool deep=!oom && message_is(ctx,exception,"Maximum call stack size exceeded");
    const char *code=oom?POCKET_ERR_OUT_OF_MEMORY:deep?POCKET_ERR_LIMIT_EXCEEDED
                                                      :POCKET_ERR_CORRUPT_DATA;
    char message[LOAD_MESSAGE_MAX];
    snprintf(message,sizeof message,"chunk '%s' %s: ",chunk->name,
             ran?"threw while it ran":"did not compile");
    if(oom) snprintf(message+strlen(message),sizeof message-strlen(message),"out of memory");
    else describe(ctx,exception,message,sizeof message);
    ESP_LOGW(TAG,"APP_LOAD %s failed %s",chunk->name,message);
    // A compile that failed declared nothing, so the chunk may be asked for
    // again -- worth it only when what failed was the heap. One that ran is
    // `failed` for good (see the masks above), so nothing about it is
    // retryable, and whether its partial run left anything behind is
    // exactly what "unknown" says.
    JSValue error=pocket_api_error(ctx,code,OP,message,oom && !ran,
                                   ran?POCKET_OUTCOME_UNKNOWN:POCKET_OUTCOME_NOT_APPLIED);
    if(JS_IsException(error)) { JS_FreeValue(ctx,exception); return error; }
    // The original, with its own stack, for a program that wants the chunk's
    // exception itself: ES2022's Error.cause, set rather than passed to the
    // constructor because pocket_api_error() builds the error.
    JS_SetPropertyStr(ctx,error,"cause",exception);
    return JS_Throw(ctx,error);
}

static JSValue js_load(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv) {
    (void)this_val;
    static const char *const OP="app.load";
    if(argc<1 || !JS_IsString(argv[0]))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                "load(name) needs a string",false,
                                POCKET_OUTCOME_NOT_APPLIED);
    size_t length=0;
    const char *text=JS_ToCStringLen(ctx,&length,argv[0]);
    if(!text) return JS_EXCEPTION;
    char name[APP_CHUNK_NAME_MAX+1];
    const bool fits=length>=1 && length<=APP_CHUNK_NAME_MAX && strlen(text)==length;
    if(fits) memcpy(name,text,length+1);
    JS_FreeCString(ctx,text);
    if(!fits)
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                "a chunk name is 1 to 31 bytes",false,
                                POCKET_OUTCOME_NOT_APPLIED);
    const int k=find(name,length);
    char message[LOAD_MESSAGE_MAX];
    if(k<0) {
        snprintf(message,sizeof message,"this app has no chunk '%s'",name);
        return pocket_api_throw(ctx,POCKET_ERR_NOT_FOUND,OP,message,false,
                                POCKET_OUTCOME_NOT_APPLIED);
    }
    if(set->chunks[k].module) {
        // Evaluated as a script it would be a SyntaxError at its first
        // `import`/`export`; and one without either would become a second,
        // unrelated copy of what the entry's import graph already holds.
        snprintf(message,sizeof message,"chunk '%s' is a module (%s); import it from the "
                 "app's module entry",name,set->chunks[k].file);
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,message,false,
                                POCKET_OUTCOME_NOT_APPLIED);
    }
    const uint32_t bit=1u<<k;
    if(loaded&bit) return JS_FALSE;
    if(loading&bit) {
        snprintf(message,sizeof message,"chunk '%s' is still loading (it was asked for "
                 "again from its own top level)",name);
        return pocket_api_throw(ctx,POCKET_ERR_CONFLICT,OP,message,false,
                                POCKET_OUTCOME_NOT_APPLIED);
    }
    if(failed&bit) {
        snprintf(message,sizeof message,"chunk '%s' threw while it ran and is not run again",name);
        return pocket_api_throw(ctx,POCKET_ERR_CORRUPT_DATA,OP,message,false,
                                POCKET_OUTCOME_NOT_APPLIED);
    }
    const app_chunk_t *chunk=&set->chunks[k];
    const size_t bytes=(size_t)(chunk->end-chunk->start-1);
    JSRuntime *rt=JS_GetRuntime(ctx);
    size_t used0=0,used1=0,used2=0,limit=0;
    JS_GetMemoryCounters(rt,&used0,&limit);
    const int64_t t0=esp_timer_get_time();
    // Compiled and run as two steps rather than one JS_Eval so that a failure
    // says which step it was: a compile that failed declared nothing and may
    // be retried, a run that failed may have declared half (the masks above).
    // The parse does not poll the interrupt handler -- a 30 KB chunk parses in
    // about 150 ms on the board, measured, inside a turn's 250 ms -- so the
    // deadline is met, if at all, at the first safepoint of the run.
    JSValue fn=JS_Eval(ctx,chunk->start,bytes,chunk->file,
                       JS_EVAL_TYPE_GLOBAL|JS_EVAL_FLAG_COMPILE_ONLY);
    if(JS_IsException(fn)) return fail(ctx,chunk,false);
    const int64_t t1=esp_timer_get_time();
    JS_GetMemoryCounters(rt,&used1,&limit);
    loading|=bit;
    JSValue result=JS_EvalFunction(ctx,fn);   // takes fn
    loading&=~bit;
    if(JS_IsException(result)) { failed|=bit; return fail(ctx,chunk,true); }
    JS_FreeValue(ctx,result);
    loaded|=bit;
    const int64_t t2=esp_timer_get_time();
    JS_GetMemoryCounters(rt,&used2,&limit);
    // Not one of CLAUDE.md's contracted markers; a line a measurement can
    // read without a diagnostic build: the compile, the run, and the guest
    // heap at each step (the parse temporaries are gone by `used1`).
    ESP_LOGI(TAG,"APP_LOAD %s bytes=%u compile_us=%lld run_us=%lld used=%u>%u>%u",
             chunk->name,(unsigned)bytes,(long long)(t1-t0),(long long)(t2-t1),
             (unsigned)used0,(unsigned)used1,(unsigned)used2);
    return JS_TRUE;
}

// ---------------------------------------------------------------- import
//
// Static `import` (docs/vm/eval-peak.md section 9): an app whose entry is a
// module reaches its .mjs chunks by name -- `import { draw } from 'scene'` --
// and each is parsed on its own, so the evaluation's parse peak is the
// largest module's rather than the whole app's (js_create_function frees a
// compilation's JSFunctionDefs only when that compilation ends; a module is
// one compilation, the loader's JS_Eval below).
//
// Resolution is open only while pocket_app_eval_module() compiles the entry.
// That is exactly the window in which QuickJS resolves static imports (the
// compile of each module resolves its own before it returns, in
// __JS_EvalInternal), so outside it the only caller left is a dynamic
// import(), which is refused: it would compile a module from a job, after
// the evaluation, into the resident module list that nothing can shrink, and
// pocket.app.load() already covers loading later -- with release.
static bool resolving;
// Loader frames on the C stack: each level is a JS_Eval whose resolution
// calls the next. A chain as long as the table would be 32 of them on the
// UI task's stack; eight is more than any split needs (an entry importing
// its parts is depth 1) and is what the refusal below enforces.
#define IMPORT_DEPTH_MAX 8
static int depth;

static int find_file(const char *file) {
    if(!set) return -1;
    for(uint32_t i=0;i<set->count;i++)
        if(!strcmp(set->chunks[i].file,file)) return (int)i;
    return -1;
}

// The specifier is a chunk name and nothing else: no relative or absolute
// paths, no URLs, nothing outside this app's chunks.txt. The normalized name
// is the chunk's file, because QuickJS looks a module up in loaded_modules by
// the name its compile was given, and that name is also what a stack shows
// (derby_scene.mjs:12), so the two must be the same string.
static char *module_normalize(JSContext *ctx, const char *base, const char *name, void *opaque) {
    (void)opaque;
    if(!resolving) {
        JS_ThrowTypeError(ctx,"import('%s') in %s: dynamic import() is not supported; "
                          "use pocket.app.load()",name,base);
        return NULL;
    }
    const int k=find(name,strlen(name));
    if(k<0) {
        JS_ThrowReferenceError(ctx,"%s imports '%s': this app has no chunk '%s' "
                               "(chunks.txt names them)",base,name,name);
        return NULL;
    }
    if(!set->chunks[k].module) {
        JS_ThrowReferenceError(ctx,"%s imports '%s': that chunk is a script (%s), not a "
                               ".mjs module; pocket.app.load('%s') reads it",
                               base,name,set->chunks[k].file,name);
        return NULL;
    }
    return js_strdup(ctx,set->chunks[k].file);
}

static JSModuleDef *module_load(JSContext *ctx, const char *file, void *opaque,
                                JSValueConst attributes) {
    (void)opaque;
    if(!JS_IsUndefined(attributes)) {
        JS_ThrowTypeError(ctx,"import of %s: import attributes (with {...}) are not supported",file);
        return NULL;
    }
    const int k=find_file(file);
    if(k<0) {   // module_normalize() only returns table files
        JS_ThrowReferenceError(ctx,"no module %s",file);
        return NULL;
    }
    if(depth>=IMPORT_DEPTH_MAX) {
        JS_ThrowRangeError(ctx,"import of %s: imports nest deeper than %d modules",
                           file,IMPORT_DEPTH_MAX);
        return NULL;
    }
    const app_chunk_t *chunk=&set->chunks[k];
    const size_t bytes=(size_t)(chunk->end-chunk->start-1);
    JSRuntime *rt=JS_GetRuntime(ctx);
    size_t used0=0,used1=0,limit=0;
    JS_GetMemoryCounters(rt,&used0,&limit);
    const int64_t t0=esp_timer_get_time();
    // Compiling a module also resolves its own imports, so this recurses
    // through module_normalize/module_load for them before it returns --
    // with this module's parse temporaries already freed.
    depth++;
    JSValue module=JS_Eval(ctx,chunk->start,bytes,chunk->file,
                           JS_EVAL_TYPE_MODULE|JS_EVAL_FLAG_COMPILE_ONLY);
    depth--;
    if(JS_IsException(module)) return NULL;   // its SyntaxError, OOM or refusal stays pending
    const int64_t t1=esp_timer_get_time();
    JS_GetMemoryCounters(rt,&used1,&limit);
    // Not a contracted marker. compile_us and the second heap figure include
    // the modules this one imports, whose own lines come first.
    ESP_LOGI(TAG,"APP_IMPORT %s bytes=%u compile_us=%lld used=%u>%u",chunk->name,
             (unsigned)bytes,(long long)(t1-t0),(unsigned)used0,(unsigned)used1);
    // The module stays referenced by ctx->loaded_modules; this value was the
    // compile's own reference (quickjs-libc's loader does the same).
    JSModuleDef *def=JS_VALUE_GET_PTR(module);
    JS_FreeValue(ctx,module);
    return def;
}

bool pocket_app_entry_is_module(void) {
    const char *entry=app_registry_current()->entry;
    const size_t n=entry?strlen(entry):0;
    return n>4 && !strcmp(entry+n-4,".mjs");
}

JSValue pocket_app_eval_module(JSContext *ctx, const char *source, size_t length,
                               const char *filename) {
    // Compile, resolving every static import (each chunk its own compile), then
    // link and run the graph: two steps so that the window above closes before
    // any module's top level runs -- an import() written there is a job.
    resolving=true;
    depth=0;
    JSValue module=JS_Eval(ctx,source,length,filename,
                           JS_EVAL_TYPE_MODULE|JS_EVAL_FLAG_COMPILE_ONLY);
    resolving=false;
    if(JS_IsException(module)) return module;
    // A module graph's evaluation answers with a promise (top-level await).
    // Without an await it is settled before JS_EvalFunction returns: the
    // module bodies' synchronous parts never park (vm-L2-design sec.11.2),
    // and the startup evaluation has no turn to yield to.
    JSValue promise=JS_EvalFunction(ctx,module);   // takes module
    if(JS_IsException(promise)) return promise;
    const JSPromiseStateEnum state=JS_PromiseState(ctx,promise);
    if(state==JS_PROMISE_FULFILLED) { JS_FreeValue(ctx,promise); return JS_UNDEFINED; }
    if(state==JS_PROMISE_REJECTED) {
        // The exception a script would have thrown, with its own stack and
        // file:line, for eval_reporting()'s EVAL_ERROR line. QuickJS has also
        // handed this rejection (and the failing module's own) to the guest's
        // rejection tracker as unhandled; nothing reports them, because a
        // failed evaluation never reaches a drain -- the session does not
        // start, and pocketjs_guest_destroy() frees the tracker's list.
        JSValue reason=JS_PromiseResult(ctx,promise);
        JS_FreeValue(ctx,promise);
        return JS_Throw(ctx,reason);
    }
    // Pending: some module awaited at its top level. Refused rather than
    // driven to completion: the rest of that module would run from the job
    // queue after the evaluation has been declared finished and frame()
    // bound, and a failure there would no longer be the evaluation's. The
    // session is not started, so the guest -- and the suspended module with
    // it -- is freed.
    JS_FreeValue(ctx,promise);
    return JS_ThrowSyntaxError(ctx,"%s: top-level await is not supported (the module graph "
                               "must finish evaluating synchronously)",filename);
}

// Only values this file enforces: the name length is checked on every call,
// and the chunk count is the width of the masks, refused at build time.
static const pocket_limit_t load_limits[] = {
    {.name="maxChunks",   .kind=POCKET_LIMIT_INT,.number=APP_CHUNKS_MAX},
    {.name="maxNameBytes",.kind=POCKET_LIMIT_INT,.number=APP_CHUNK_NAME_MAX},
    {0},
};

// supported=true on every session, including those whose app has no chunks:
// the function is implemented and answers NOT_FOUND for them, which is the
// honest result of asking this app for a chunk it does not have.
static const pocket_capability_t load_capability = {
    .name="app.load", .supported=true, .available=true, .limits=load_limits,
};

static esp_err_t build_load(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    JS_DefinePropertyValueStr(ctx,ns,"load",JS_NewCFunction(ctx,js_load,"load",1),
                              JS_PROP_ENUMERABLE);
    return ESP_OK;
}

esp_err_t pocket_app_load_install(JSContext *ctx) {
    pocket_api_register(&load_capability);
    set=app_chunks_for(app_registry_current()->id);
    loaded=failed=loading=0;
    resolving=false;
    // The runtime's one module-loader slot (nothing else registers one: the
    // guest leaves it empty and quickjs-libc sets it only for workers).
    // Registered for every session, chunks or not, so that a dynamic import()
    // anywhere meets the same refusal rather than QuickJS's "could not load".
    JS_SetModuleLoaderFunc2(JS_GetRuntime(ctx),module_normalize,module_load,NULL,NULL);
    return pocket_api_lazy(ctx,"app",build_load,NULL);
}
