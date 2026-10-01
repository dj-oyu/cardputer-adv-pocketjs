/* Compile the actual production entry, guest and console. Host sizes/timer
 * shims do not establish ESP32 timing, heap margins or display correctness. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pocketjs/guest_quickjs.h"
#include "quickjs-vm.h"
#include "jsconsole.h"
#include "esp_log.h"

extern void vmtest_vm_set_force_yield(JSRuntime *, int);
static pocketjs_guest_t *guest;
static const char *user_prelude;
static size_t user_prelude_length;
static unsigned reports, heap_live, allocs, fail_at, failures;
static JSValue reported;
static bool poison_report;

void *frame_entry_heap_malloc(size_t size) {
    if (++allocs == fail_at) { failures++; return NULL; }
    void *p = malloc(size);
    if (p) heap_live++;
    return p;
}
void frame_entry_heap_free(void *p) {
    if (p) { assert(heap_live); heap_live--; }
    free(p);
}
int64_t esp_timer_get_time(void) {
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
/* The loader and OOM logging are deliberately outside this source-entry test.
 * In particular this stub must never make a module test look like a pass. */
static JSValue pocket_app_eval_module(JSContext *ctx, const char *source,
                                     size_t length, const char *name) {
    (void)ctx; (void)source; (void)length; (void)name;
    assert(!"production module loader is not linked");
    return JS_EXCEPTION;
}
static void report_oom_if_any(void) {}
#include "frame_entry_source.inc"

static JSContext *context(void) { return pocketjs_guest_quickjs_context(guest); }
static JSRuntime *runtime(void) { return JS_GetRuntime(context()); }
static JSValue eval(const char *s) {
    JSValue v = JS_Eval(context(), s, strlen(s), "check.js", JS_EVAL_TYPE_GLOBAL);
    assert(!JS_IsException(v));
    return v;
}
static void check(const char *s) {
    JSValue v = eval(s);
    assert(JS_ToBool(context(), v) == 1);
    JS_FreeValue(context(), v);
}
static void report(JSContext *ctx, JSValueConst exception, void *opaque) {
    assert(opaque == &reports);
    assert(!JS_VMSuspended(JS_GetRuntime(ctx)));
    reports++;
    JS_FreeValue(ctx, reported);
    reported = JS_DupValue(ctx, exception);
    frame_error(ctx, exception, NULL); /* Production callback, not a copy. */
    if (poison_report) JS_ThrowInternalError(ctx, "reporter failed");
}
static void create(void) {
    assert(!guest && !heap_live);
    pocketjs_guest_config_t cfg;
    pocketjs_guest_config_defaults(&cfg);
    cfg.prefer_psram = false; /* One injected refusal = one allocator attempt. */
    assert(pocketjs_guest_create(&cfg, &guest) == ESP_OK);
    pocketjs_guest_yield_enabled(guest, false);
    pocketjs_guest_set_frame_error_handler(guest, report, &reports);
    assert(pocketjs_guest_quickjs_install_once(guest, "console", jsconsole_install, NULL) == ESP_OK);
    user_prelude = NULL; user_prelude_length = 0;
    reports = 0; reported = JS_UNDEFINED; poison_report = false;
    jsconsole_clear();
}
static void destroy(void) {
    fail_at = 0;
    vmtest_vm_set_force_yield(runtime(), 0);
    JS_FreeValue(context(), reported);
    pocketjs_guest_destroy(guest);
    guest = NULL;
    assert(heap_live == 0);
}
static void source(const char *s) {
    assert(eval_user_source(s, strlen(s), false) == ESP_OK);
}
static unsigned finish(esp_err_t *err) {
    unsigned resumes = 0;
    while (*err == ESP_OK && pocketjs_guest_work_pending(guest)) {
        assert(!reports);
        assert(++resumes < 10000);
        JS_RunGC(runtime()); /* Parked receiver, argv, closures must be rooted. */
        *err = pocketjs_guest_continue(guest);
    }
    return resumes;
}
static const pocketjs_guest_frame_t empty_input = {.struct_size = sizeof(empty_input)};
static esp_err_t call(void) {
    esp_err_t err=pocketjs_guest_frame(guest,&empty_input);
    finish(&err); return err;
}

static void loops(void) {
    const char *programs[] = {
        "globalThis.sum=0;globalThis.frame=function(){for(let i=0;i<100;i++)sum+=i;}",
        "globalThis.sum=0;globalThis.frame=function(){for(let i=0;i<1000;i++)sum+=i;}",
        "globalThis.sum=0;globalThis.frame=function(){'use strict';if(this!==globalThis)throw Error('receiver');for(let i=0;i<100;i++)sum+=i;if(this!==globalThis)throw Error('resumed receiver');}",
        "globalThis.sum=0;globalThis.frame=function(){for(let i=0;i<100;i++)sum+=i;throw new Error('boom');}",
        "globalThis.sum=0;globalThis.frame=function(){throw new Error('boom');}",
    };
    const int counts[] = {100, 1000, 100, 100, 0};
    for (unsigned i = 0; i < sizeof(programs)/sizeof(*programs); i++) {
        create(); source(programs[i]);
        vmtest_vm_set_force_yield(runtime(), counts[i] != 0);
        esp_err_t err = pocketjs_guest_frame(guest, &empty_input);
        unsigned resumes = finish(&err);
        assert(err == (i >= 3 ? ESP_FAIL : ESP_OK));
        assert(reports == (unsigned)(i >= 3));
        char assertion[100];
        snprintf(assertion, sizeof(assertion), "sum===%d", counts[i]*(counts[i]-1)/2);
        check(assertion);
        assert(resumes >= (unsigned)(counts[i]*2));
        if (i >= 3) assert(strstr(jsconsole_error(), "boom") && strstr(jsconsole_error(), "user.js"));
        assert(!JS_HasException(context()));
        printf("FRAME_ENTRY loop=%d resumes=%u reports=%u PASS\n", counts[i], resumes, reports);
        destroy();
    }
    /* Negative control: restore the old wrapper and prove the same loop no
     * longer parks. A runner that cannot distinguish it proves nothing. */
    create(); source(programs[0]);
    source("(function(){var f=globalThis.frame;if(typeof f!=='function')return;globalThis.frame=function(){try{return f.apply(this,arguments);}catch(e){__pjs_error(String(e),e&&e.stack);throw e;}};})()");
    vmtest_vm_set_force_yield(runtime(), 1);
    esp_err_t err = pocketjs_guest_frame(guest, &empty_input);
    unsigned resumes=finish(&err);
    printf("FRAME_ENTRY old-wrapper resumes=%u\n",resumes);
    assert(resumes == 1 && err == ESP_OK);
    check("sum===4950"); destroy();
    puts("FRAME_ENTRY old-wrapper negative control PASS");
}

static JSValue request_yield(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self; (void)argc; (void)argv;
    JS_VMRequestYield(JS_GetRuntime(ctx));
    return JS_UNDEFINED;
}
static void requested_yield(void) {
    create();
    JSValue global=JS_GetGlobalObject(context());
    assert(JS_SetPropertyStr(context(),global,"requestYield",
        JS_NewCFunction(context(),request_yield,"requestYield",0))>=0);
    JS_FreeValue(context(),global);
    source("globalThis.sum=0;frame=()=>{requestYield();for(let i=0;i<100;i++)sum+=i}");
    esp_err_t err=pocketjs_guest_frame(guest,&empty_input);
    assert(pocketjs_guest_suspended(guest));
    assert(finish(&err)==1 && err==ESP_OK); check("sum===4950"); destroy();
    puts("FRAME_ENTRY actual yield-request API (force hook off) PASS");
}

static void arguments_and_repeat(void) {
    const uint32_t touches[] = {0, UINT32_MAX, 0x12345678};
    const int32_t hits[] = {-1, INT32_MIN, INT32_MAX};
    const pocketjs_guest_frame_t input = {.struct_size=sizeof(input),
        .buttons=UINT32_MAX, .analog=0x8080, .touches=touches,
        .touch_hits=hits, .touch_count=3};
    const char *body =
        "globalThis.calls=0;globalThis.frame=function(b,a,t,h){'use strict';"
        "if(this!==globalThis||arguments.length!==4||b!==4294967295||a!==32896)throw Error('args');"
        "for(let i=0;i<3;i++){}"
        "if(t.length!==3||t[1]!==4294967295||t[2]!==305419896||h[0]!==-1||h[1]!==-2147483648||h[2]!==2147483647)throw Error('retained args');"
        "if(this!==globalThis)throw Error('retained this');calls++;};";
    create(); source(body); vmtest_vm_set_force_yield(runtime(), 1);
    for (int n=0; n<40; n++) {
        esp_err_t err = pocketjs_guest_frame(guest, &input);
        assert(pocketjs_guest_suspended(guest));
        assert(pocketjs_guest_frame(guest, &input) == ESP_ERR_INVALID_STATE);
        assert(finish(&err) > 0 && err == ESP_OK);
        assert(!JS_HasException(context()));
    }
    check("calls===40");
    pocketjs_guest_stats_t stats = {.struct_size=sizeof(stats)};
    assert(pocketjs_guest_stats(guest, &stats) == ESP_OK);
    assert(stats.frames == 40 && stats.frame_errors == 0);
    source("frame=function(b,a,t){if(arguments.length!==3||t.length!==3)throw Error('three args')}");
    pocketjs_guest_frame_t three = input; three.touch_hits = NULL;
    esp_err_t err=pocketjs_guest_frame(guest, &three);
    finish(&err); assert(err==ESP_OK);
    source("frame=function(){if(arguments.length!==2)throw Error('two args')}");
    err=pocketjs_guest_frame(guest, &empty_input); finish(&err); assert(err==ESP_OK);
    destroy(); puts("FRAME_ENTRY argv/receiver GC retention + 40 repeated calls PASS");
}

static void jobs_and_callables(void) {
    /* Pending jobs must not run while the frame that queued them is parked. */
    create(); source("globalThis.order='';globalThis.frame=function(){order+='a';Promise.resolve().then(()=>{order+='c';for(let i=0;i<3;i++){}order+='d';});for(let i=0;i<3;i++){}order+='b';}");
    vmtest_vm_set_force_yield(runtime(), 1);
    esp_err_t err = pocketjs_guest_frame(guest, &empty_input);
    assert(pocketjs_guest_suspended(guest));
    /* No new JS entry while parked: inspect globals only after finish. */
    assert(finish(&err) > 0 && err == ESP_OK);
    check("order==='abcd'");
    assert(!JS_IsJobPending(runtime())); destroy();

    const char *callables[] = {
        "globalThis.calls=0;globalThis.frame=async function(b){if(this!==globalThis||b!==0)throw Error('async this');for(let i=0;i<3;i++){}await 0;for(let i=0;i<3;i++){}calls++;}",
        "globalThis.calls=0;globalThis.frame=(function(x,b){'use strict';if(this.token!==7||x!==9||b!==0)throw Error('bound');for(let i=0;i<3;i++){}calls++;}).bind({token:7},9)",
        "globalThis.calls=0;globalThis.frame=new Proxy(function(b){'use strict';if(this!==globalThis||b!==0)throw Error('proxy');for(let i=0;i<3;i++){}calls++;},{})",
        "globalThis.calls=0;globalThis.frame=()=>{if(this!==globalThis)throw Error('arrow');for(let i=0;i<3;i++){}calls++;}",
    };
    for (unsigned i=0; i<sizeof(callables)/sizeof(*callables); i++) {
        create(); source(callables[i]); vmtest_vm_set_force_yield(runtime(), 1);
        err=pocketjs_guest_frame(guest, &empty_input);
        unsigned resumes=finish(&err);
        assert(err==ESP_OK && !reports); check("calls===1");
        printf("FRAME_ENTRY callable=%u resumes=%u PASS\n", i, resumes);
        destroy();
    }
    create(); source("globalThis.order='';frame=()=>{order+='a';Promise.resolve().then(()=>order+='b');Promise.resolve().then(()=>order+='c')}");
    vm_budget_t budget; vm_budget_begin_full(&budget,0,1,0,1);
    pocketjs_guest_budget(guest,&budget);
    err=pocketjs_guest_frame(guest,&empty_input);
    assert(pocketjs_guest_jobs_pending(guest)&&!pocketjs_guest_suspended(guest));
    assert(finish(&err)==1 && err==ESP_OK); check("order==='abc'"); destroy();

    /* Job failures are not synchronous frame failures (same as old wrapper). */
    const char *rejections[]={
        "frame=()=>{Promise.resolve().then(()=>{for(let i=0;i<3;i++){}throw Error('job boom')})}",
        "frame=async()=>{for(let i=0;i<3;i++){}throw Error('async frame boom')}",
        "frame=async()=>{await 0;for(let i=0;i<3;i++){}throw Error('await frame boom')}",
    };
    for(unsigned i=0;i<sizeof(rejections)/sizeof(*rejections);i++) {
        create(); source(rejections[i]); vmtest_vm_set_force_yield(runtime(),1);
        err=pocketjs_guest_frame(guest,&empty_input); finish(&err);
        assert(err==ESP_FAIL && reports==0 && !JS_HasException(context())); destroy();
    }
    puts("FRAME_ENTRY pending jobs and async rejection routing PASS");
}

static void errors(void) {
    const char *throws[] = {
        "throw 'text'", "throw null", "throw undefined", "throw 42",
        "throw Symbol('token')", "throw Symbol()",
        "throw {toString(){throw Error('string getter')},get stack(){throw Error('stack getter')}}",
        "throw {toString(){return 'original'},stack:{toString(){throw Error('stack conversion')}}}",
        "let e=Error('original');Object.defineProperty(e,'stack',{get(){throw Error('stack getter')}});throw e",
    };
    const char *messages[]={"text","null","undefined","42","Symbol(token)","Symbol()",
        "frame failed","original","Error: original"};
    for(unsigned i=0;i<sizeof(throws)/sizeof(*throws);i++) {
        for(unsigned parked=0;parked<2;parked++) {
            create(); char text[512];
            snprintf(text,sizeof(text),"frame=function(){for(let i=0;i<3;i++){}%s}",throws[i]);
            source(text); poison_report=true; vmtest_vm_set_force_yield(runtime(),parked);
            esp_err_t err=pocketjs_guest_frame(guest,&empty_input); unsigned resumes=finish(&err);
            assert(err==ESP_FAIL && reports==1 && jsconsole_error());
            assert(strstr(jsconsole_error(),messages[i]));
            assert((resumes>0)==(parked!=0));
            assert(!JS_HasException(context()));
            /* A formatter's exception must not poison a later call. */
            poison_report=false; reports=0; source("globalThis.ok=0;frame=()=>{ok++}");
            assert(call()==ESP_OK); check("ok===1");
            destroy();
        }
    }
    puts("FRAME_ENTRY primitive/hostile exception + recovery cases=18 PASS");
}

static void formatting_compatibility(void) {
    const char *programs[]={
        "frame=function(){throw Error('message')}",
        "frame=function(){throw 'plain'}",
        "frame=function(){throw Symbol('label')}",
        "frame=function(){throw 0}",
        "frame=function(){throw false}",
        "frame=function(){throw ''}",
        "frame=function(){throw NaN}",
        "frame=function(){throw null}",
        "frame=function(){throw undefined}",
        "String.prototype.stack='primitive-location';frame=function(){throw 'plain'}",
        "frame=function(){throw {toString(){return 'object'},stack:'location\\nignored'}}",
    };
    const char *wrap="(function(){var f=globalThis.frame;if(typeof f!=='function')return;globalThis.frame=function(){try{return f.apply(this,arguments);}catch(e){__pjs_error(String(e),e&&e.stack);throw e;}};})()";
    for(unsigned i=0;i<sizeof(programs)/sizeof(*programs);i++) {
        char direct[128];
        create(); source(programs[i]); assert(call()==ESP_FAIL);
        snprintf(direct,sizeof(direct),"%s",jsconsole_error()); destroy();
        create(); source(programs[i]); source(wrap);
        pocketjs_guest_set_frame_error_handler(guest,NULL,NULL);
        assert(call()==ESP_FAIL); assert(strcmp(direct,jsconsole_error())==0);
        destroy();
    }
    puts("FRAME_ENTRY old-wrapper display byte compatibility cases=11 PASS");
}
static int watchdog(void *opaque) {
    unsigned *polls=opaque;
    return ++*polls>=2;
}
static void watchdog_interrupt(void) {
    for(unsigned parked=0;parked<2;parked++) {
        create(); source("globalThis.started=0;globalThis.caught=0;globalThis.finished=0;frame=()=>{started++;try{for(;;){}}catch(e){caught++}finally{finished++}}");
        if(parked) {
            vmtest_vm_set_force_yield(runtime(),1);
            assert(pocketjs_guest_frame(guest,&empty_input)==ESP_OK);
            assert(pocketjs_guest_suspended(guest));
            vmtest_vm_set_force_yield(runtime(),0);
        }
        unsigned polls=0; pocketjs_guest_set_watchdog(guest,watchdog,&polls);
        esp_err_t err=parked?pocketjs_guest_continue(guest):pocketjs_guest_frame(guest,&empty_input);
        assert(err==ESP_FAIL && !pocketjs_guest_suspended(guest));
        assert(reports==1 && strstr(jsconsole_error(),"interrupted"));
        pocketjs_guest_set_watchdog(guest,NULL,NULL);
        assert(!JS_HasException(context()));
        check("started===1&&caught===0&&finished===0"); destroy();
    }
    puts("FRAME_ENTRY uncatchable watchdog direct/resumed PASS");
}

static void entry_modes(void) {
    create();
    const char *direct="globalThis.direct=0;frame=function(){'use strict';if(this!==undefined)throw Error('direct receiver');direct++}";
    assert(pocketjs_guest_eval(guest,direct,strlen(direct),"direct.js")==ESP_OK);
    vmtest_vm_set_force_yield(runtime(),1);
    assert(call()==ESP_OK); check("direct===1"); destroy();
    create(); user_prelude="const shared=7"; user_prelude_length=strlen(user_prelude);
    source("globalThis.answer=0;frame=()=>{answer=shared}");
    assert(call()==ESP_OK); check("answer===7"); destroy();
    create(); const char *s="let x=1";
    assert(eval_user_source(s,strlen(s),false)==ESP_ERR_NOT_FOUND);
    source("frame=null;globalThis.jobs=0;Promise.resolve().then(()=>jobs++)");
    check("jobs===1"); JS_FreeValue(context(),eval("Promise.resolve().then(()=>jobs++)"));
    assert(call()==ESP_OK); check("jobs===2"); destroy();
    create(); s="throw Error('top-level')";
    assert(eval_user_source(s,strlen(s),false)==ESP_FAIL);
    assert(strstr(jsconsole_error(),"top-level")&&strstr(jsconsole_error(),"user.js"));
    assert(!reports); destroy();
    puts("FRAME_ENTRY production prelude/missing/null/top-level error PASS");
}

static void stopping(void) {
    const char *shapes[]={
        "frame=()=>work()",
        "frame=()=>{Promise.resolve().then(work)}",
        "frame=async()=>{await 0;work()}",
        "frame=()=>{async function* g(){await 0;work();yield 1}g().next()}",
    };
    for(unsigned shape=0;shape<sizeof(shapes)/sizeof(*shapes);shape++) {
      for (unsigned mode=0;mode<3;mode++) {
        create(); source("globalThis.side=0;globalThis.finallyRan=0;function work(){try{for(;;){side++}}finally{finallyRan++}};frame=null");
        source(shapes[shape]); vmtest_vm_set_force_yield(runtime(),1);
        assert(pocketjs_guest_frame(guest,&empty_input)==ESP_OK);
        assert(pocketjs_guest_suspended(guest)); JS_RunGC(runtime());
        if(mode!=2) {
            if(mode==1) { JS_SetMemoryLimit(runtime(),0); JS_SetMaxStackSize(runtime(),1); }
            pocketjs_guest_prepare_stop(guest);
            assert(!pocketjs_guest_suspended(guest)&&!reports);
            JS_SetMemoryLimit(runtime(),4*1024*1024); JS_SetMaxStackSize(runtime(),256*1024);
            assert(!JS_HasException(context())); check("finallyRan===0");
            /* Stop hooks may enter JS even when their old jobs remain queued. */
            JS_FreeValue(context(),eval("side=42")); check("side===42");
        }
        destroy();
      }
    }
    puts("FRAME_ENTRY terminate/zero-heap+stack/destroy parked cases=12 PASS");
}

static void oom_sweep(bool warm_builtins) {
    const uint32_t touches[]={1,2,3}; const int32_t hits[]={-1,-2,-3};
    const pocketjs_guest_frame_t input={.struct_size=sizeof(input),.touches=touches,.touch_hits=hits,.touch_count=3};
    const char *s="globalThis.done=0;frame=(b,a,t,h)=>{let objects=[];for(let i=0;i<8;i++)objects.push({i,t,h});done=objects.length}";
    unsigned attempts=0, rejected=0, passed=0;
    for(unsigned mode=0;mode<2;mode++) {
        /* Measure this path, then fail each allocation once in a fresh realm. */
        create(); source(s);
        if(warm_builtins) JS_FreeValue(context(),eval("[].push(0)"));
        vmtest_vm_set_force_yield(runtime(),mode);
        allocs=0; esp_err_t err=pocketjs_guest_frame(guest,&input); finish(&err);
        assert(err==ESP_OK); unsigned total=allocs; destroy(); attempts+=total;
        for(unsigned n=1;n<=total;n++) {
            create(); source(s);
            if(warm_builtins) JS_FreeValue(context(),eval("[].push(0)"));
            vmtest_vm_set_force_yield(runtime(),mode);
            allocs=0; failures=0; fail_at=n;
            err=pocketjs_guest_frame(guest,&input); finish(&err);
            fail_at=0; assert(failures==1);
            assert(!pocketjs_guest_suspended(guest));
            if(JS_HasException(context())) fprintf(stderr,"OOM_PENDING mode=%u nth=%u err=%d reports=%u total=%u\n",mode,n,err,reports,total);
            assert(!JS_HasException(context()));
            if(err==ESP_OK) { passed++; check("done===8"); }
            else { rejected++; assert(err==ESP_FAIL||err==ESP_ERR_NO_MEM); }
            /* A refusal must not strand argv, a VM frame or pending error. */
            reports=0; source("frame=()=>{done=99}");
            assert(call()==ESP_OK); check("done===99");
            destroy();
        }
    }
    printf("FRAME_ENTRY OOM allocation sweep attempts=%u rejected=%u recovered=%u PASS\n",attempts,rejected,passed);
}
int main(int argc, char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    puts("FRAME_ENTRY START");
    if(argc==2 && strcmp(argv[1],"--oom-lazy")==0) { oom_sweep(false); return 0; }
    if(argc!=1) { fputs("usage: test-frame-entry [--oom-lazy]\n",stderr); return 2; }
    loops(); requested_yield(); arguments_and_repeat(); jobs_and_callables(); errors();
    formatting_compatibility(); watchdog_interrupt();
    entry_modes(); stopping(); oom_sweep(true);
    puts("FRAME_ENTRY ALL PASS");
    return 0;
}
