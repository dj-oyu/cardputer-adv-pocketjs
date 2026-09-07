#include "pocket_api.h"
#include "board.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

// How many capabilities may be registered in total -- not headroom above
// builtins[], which is what the old wording said and what made 16 look
// generous. Nineteen names register today and the sixteenth filled the table:
// time, log and pet.companion silently reported supported=false while working,
// which is the honesty mechanism failing at exactly the thing it exists for.
// A pointer each, so the cost of the margin is 64 bytes of .bss.
#define POCKET_MAX_REGISTERED    32
#define POCKET_MAX_SUBSCRIPTIONS 8

// How many namespace contributors may register. Eighteen names have one each
// and two names have two, so twenty is what is used today; the margin costs
// twelve bytes of .bss per unused slot and the alternative is the same silent
// truncation the capability table was found doing.
#define POCKET_MAX_LAZY 28

// The names of docs/common-api.md section 2, all of them. A name that is not
// implemented yet still has to answer get() with supported=false rather than
// throw, so the whole list is declared here and later stages replace entries
// through pocket_api_register().
//
// supported means "this firmware implements the pocket.* surface for it". None
// of these surfaces exist yet: stage A ships the foundation only, so every
// entry is false and an app that feature-tests before calling gets an honest
// answer instead of a TypeError.
static const pocket_capability_t builtins[] = {
    {.name="ui.basic",         .reason=POCKET_REASON_NOT_IMPLEMENTED},
    {.name="input.text",       .reason=POCKET_REASON_NOT_IMPLEMENTED},
    {.name="storage.kv",       .reason=POCKET_REASON_NOT_IMPLEMENTED},
    {.name="fs.volume.app",    .reason=POCKET_REASON_NOT_IMPLEMENTED},
    {.name="fs.volume.assets", .reason=POCKET_REASON_NOT_IMPLEMENTED},
    {.name="fs.volume.sd",     .reason=POCKET_REASON_NOT_IMPLEMENTED},
    {.name="sensors.imu",      .reason=POCKET_REASON_NOT_IMPLEMENTED},
    {.name="audio.tone",       .reason=POCKET_REASON_NOT_IMPLEMENTED},
    {.name="audio.capture",    .reason=POCKET_REASON_NOT_IMPLEMENTED},
    {.name="audio.playback",   .reason=POCKET_REASON_NOT_IMPLEMENTED},
    {.name="net.wifi",         .reason=POCKET_REASON_NOT_IMPLEMENTED},
    {.name="net.http",         .reason=POCKET_REASON_NOT_IMPLEMENTED},
    {.name="ble.central",      .reason=POCKET_REASON_NOT_IMPLEMENTED},
    {.name="ble.peripheral",   .reason=POCKET_REASON_NOT_IMPLEMENTED},
    {.name="io.i2c",           .reason=POCKET_REASON_NOT_IMPLEMENTED},
    {.name="io.spi",           .reason=POCKET_REASON_NOT_IMPLEMENTED},
    {.name="io.uart",          .reason=POCKET_REASON_NOT_IMPLEMENTED},
    {.name="io.gpio",          .reason=POCKET_REASON_NOT_IMPLEMENTED},
    {.name="io.ir",            .reason=POCKET_REASON_NOT_IMPLEMENTED},
    {.name="bridge.pc",        .reason=POCKET_REASON_NOT_IMPLEMENTED},
};

static const char *const error_codes[] = {
    POCKET_ERR_INVALID_ARGUMENT, POCKET_ERR_UNSUPPORTED,
    POCKET_ERR_NOT_AVAILABLE,    POCKET_ERR_PERMISSION_DENIED,
    POCKET_ERR_BUSY,             POCKET_ERR_LIMIT_EXCEEDED,
    POCKET_ERR_OUT_OF_MEMORY,    POCKET_ERR_TIMEOUT,
    POCKET_ERR_CANCELLED,        POCKET_ERR_CLOSED,
    POCKET_ERR_DISCONNECTED,     POCKET_ERR_NOT_FOUND,
    POCKET_ERR_CORRUPT_DATA,     POCKET_ERR_IO_ERROR,
    POCKET_ERR_AUTH_FAILED,      POCKET_ERR_TLS_ERROR,
    POCKET_ERR_CONFLICT,
};

static const pocket_capability_t *overrides[POCKET_MAX_REGISTERED];
static unsigned                   override_count;

// How deep the completion table is. The low bits of a request number are its
// slot's index, so this has to stay a power of two.
//
// A slot costs 72 bytes of .bss, measured, and today exactly one surface uses
// one, so the table is sized for the concurrency that is coming -- IR transmit,
// audio.player and HTTP, which unlike a tone do not exclude each other -- and
// not for the concurrency anyone has asked for. Raising the bits by one doubles
// the table and nothing else; the request number rearranges itself around it.
#define POCKET_PROMISE_SLOT_BITS 2
#define POCKET_MAX_PROMISES      (1u<<POCKET_PROMISE_SLOT_BITS)

// Per-realm state. It lives in a hub object the pocket namespace holds, so the
// realm's finalizer is what ends its life: nothing here outlives the guest.
typedef struct {
    JSContext         *ctx;
    JSValue            error_proto;
    pocket_sub_slot_t  subscribers[POCKET_MAX_SUBSCRIPTIONS];
    pocket_sub_table_t subs;
} pocket_state_t;

typedef struct {
    bool cancelled;
} pocket_cancel_t;

static pocket_state_t *state;
static JSClassID       hub_class;
static JSClassID       token_class;

static const char *TAG = "pocket";

// ---------------------------------------------------------------- registry

static const pocket_capability_t *lookup(const char *name) {
    // Newest registration wins, so a later stage can replace a declared entry.
    for(unsigned i=override_count;i>0;i--)
        if(!strcmp(overrides[i-1]->name,name)) return overrides[i-1];
    for(unsigned i=0;i<sizeof(builtins)/sizeof(builtins[0]);i++)
        if(!strcmp(builtins[i].name,name)) return &builtins[i];
    return NULL;
}

bool pocket_api_supported(const char *name) {
    const pocket_capability_t *cap=name?lookup(name):NULL;
    return cap && cap->supported;
}

esp_err_t pocket_api_register(const pocket_capability_t *capability) {
    if(!capability || !capability->name) return ESP_ERR_INVALID_ARG;
    for(unsigned i=0;i<override_count;i++) {
        if(!strcmp(overrides[i]->name,capability->name)) {
            overrides[i]=capability;
            return ESP_OK;
        }
    }
    if(override_count>=POCKET_MAX_REGISTERED) {
        // Loud, because eighteen of the nineteen call sites discard this
        // return value and the symptom is a capability that works while
        // denying it exists -- which no app can distinguish from one that is
        // genuinely absent.
        ESP_LOGE(TAG,"capability table full at %u; \"%s\" will report "
                     "supported=false while it works",
                 POCKET_MAX_REGISTERED,capability->name);
        return ESP_ERR_NO_MEM;
    }
    overrides[override_count++]=capability;
    return ESP_OK;
}

// ------------------------------------------------------------ error shape

JSValue pocket_api_error(JSContext *ctx, const char *code, const char *operation,
                         const char *message, bool retryable, const char *outcome) {
    // JS_NewError fills in the stack, which is the part a plain object cannot
    // reproduce; the prototype swap is what turns it into a PocketError.
    JSValue error=JS_NewError(ctx);
    if(JS_IsException(error)) return error;
    if(state && !JS_IsUndefined(state->error_proto))
        JS_SetPrototype(ctx,error,state->error_proto);
    JS_SetPropertyStr(ctx,error,"message",JS_NewString(ctx,message?message:code));
    JS_SetPropertyStr(ctx,error,"code",JS_NewString(ctx,code));
    JS_SetPropertyStr(ctx,error,"operation",
                      JS_NewString(ctx,operation?operation:""));
    JS_SetPropertyStr(ctx,error,"retryable",JS_NewBool(ctx,retryable));
    if(outcome) JS_SetPropertyStr(ctx,error,"outcome",JS_NewString(ctx,outcome));
    return error;
}

JSValue pocket_api_throw(JSContext *ctx, const char *code, const char *operation,
                         const char *message, bool retryable, const char *outcome) {
    JSValue error=pocket_api_error(ctx,code,operation,message,retryable,outcome);
    if(JS_IsException(error)) return error;
    return JS_Throw(ctx,error);
}

JSValue pocket_api_settled(JSContext *ctx, JSValue value, bool rejected) {
    JSValue funcs[2];
    JSValue promise=JS_NewPromiseCapability(ctx,funcs);
    if(JS_IsException(promise)) { JS_FreeValue(ctx,value); return promise; }
    JSValue done=JS_Call(ctx,funcs[rejected?1:0],JS_UNDEFINED,1,
                         (JSValueConst *)&value);
    JS_FreeValue(ctx,done);
    JS_FreeValue(ctx,funcs[0]);
    JS_FreeValue(ctx,funcs[1]);
    JS_FreeValue(ctx,value);
    return promise;
}

JSValue pocket_api_reject(JSContext *ctx, const char *code, const char *operation,
                          const char *message, bool retryable, const char *outcome) {
    JSValue error=pocket_api_error(ctx,code,operation,message,retryable,outcome);
    if(JS_IsException(error)) return error;   // only on OOM building the error
    return pocket_api_settled(ctx,error,true);
}

// --------------------------------------------------------- cancel tokens

static void token_finalizer(JSRuntime *rt, JSValueConst value) {
    (void)rt;
    free(JS_GetOpaque(value,token_class));
}

bool pocket_api_is_cancel_token(JSValueConst value) {
    return JS_GetOpaque(value,token_class)!=NULL;
}

bool pocket_api_cancel_requested(JSValueConst token) {
    const pocket_cancel_t *c=JS_GetOpaque(token,token_class);
    return c && c->cancelled;
}

static JSValue js_cancel(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv, int magic,
                         JSValueConst *func_data) {
    (void)ctx; (void)this_val; (void)argc; (void)argv; (void)magic;
    pocket_cancel_t *c=JS_GetOpaque(func_data[0],token_class);
    if(c) c->cancelled=true;
    return JS_UNDEFINED;
}

static JSValue js_cancel_source(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    pocket_cancel_t *c=calloc(1,sizeof(*c));
    if(!c) return pocket_api_throw(ctx,POCKET_ERR_OUT_OF_MEMORY,"cancel.source",
                                   "no memory for a cancel token",false,NULL);
    JSValue token=JS_NewObjectClass(ctx,token_class);
    if(JS_IsException(token)) { free(c); return token; }
    JS_SetOpaque(token,c);
    JSValue source=JS_NewObject(ctx);
    if(JS_IsException(source)) { JS_FreeValue(ctx,token); return source; }
    JSValue cancel=JS_NewCFunctionData(ctx,js_cancel,0,0,1,&token);
    JS_SetPropertyStr(ctx,source,"token",token);
    JS_SetPropertyStr(ctx,source,"cancel",cancel);
    return source;
}

// ----------------------------------------------------------- subscriptions

// close() is bound to its table, its slot and the handle that slot held when it
// was made, so a second call, or a call after the slot has been reused, finds a
// mismatch and does nothing. func_data is the C function's own storage and is
// not reachable from JS, so the table address it carries cannot be forged; the
// table also outlives every close() bound to it, because a close() is an object
// of the realm whose teardown is what ends a per-realm table's life.
static JSValue js_sub_close(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv, int magic,
                            JSValueConst *func_data) {
    (void)this_val; (void)argc; (void)argv;
    uint32_t handle=0;
    int64_t  address=0;
    if(JS_ToUint32(ctx,&handle,func_data[0])) return JS_EXCEPTION;
    if(JS_ToInt64(ctx,&address,func_data[1])) return JS_EXCEPTION;
    pocket_sub_table_t *table=(pocket_sub_table_t *)(uintptr_t)address;
    if(table && magic>=0 && magic<table->count && handle &&
       table->slots[magic].handle==handle)
        pocket_api_sub_close(table,magic);
    return JS_UNDEFINED;
}

JSValue pocket_api_sub_open(JSContext *ctx, pocket_sub_table_t *table,
                            JSValueConst listener, const char *operation,
                            const char *full, int *slot_out) {
    int slot=-1;
    for(int i=0;i<table->count;i++)
        if(!table->slots[i].handle) { slot=i; break; }
    if(slot<0) return pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,operation,
                                       full,false,NULL);
    JSValue subscription=JS_NewObject(ctx);
    if(JS_IsException(subscription)) return subscription;
    if(++table->next_handle==0) table->next_handle=1;
    table->slots[slot].handle=table->next_handle;
    table->slots[slot].callback=JS_DupValue(ctx,listener);
    table->ctx=ctx;
    table->open++;
    if(table->changed) table->changed(table);
    JSValue data[2]={JS_NewUint32(ctx,table->slots[slot].handle),
                     JS_NewInt64(ctx,(int64_t)(uintptr_t)table)};
    JSValue close=JS_NewCFunctionData(ctx,js_sub_close,0,slot,2,data);
    JS_FreeValue(ctx,data[0]);
    JS_FreeValue(ctx,data[1]);
    JS_SetPropertyStr(ctx,subscription,"close",close);
    if(slot_out) *slot_out=slot;
    return subscription;
}

void pocket_api_sub_close(pocket_sub_table_t *table, int slot) {
    if(slot<0 || slot>=table->count || !table->slots[slot].handle) return;
    table->slots[slot].handle=0;
    // A table whose realm never opened anything holds no JS value to free.
    if(table->ctx) JS_FreeValue(table->ctx,table->slots[slot].callback);
    table->slots[slot].callback=JS_UNDEFINED;
    table->open--;
    if(table->changed) table->changed(table);
}

void pocket_api_sub_close_all(pocket_sub_table_t *table) {
    for(int i=0;i<table->count;i++) pocket_api_sub_close(table,i);
}

void pocket_api_sub_deliver(pocket_sub_table_t *table,
                            pocket_sub_payload_fn build, void *user) {
    JSContext *ctx=table->ctx;
    if(!ctx) return;
    for(int i=0;i<table->count;i++) {
        // A listener may close subscriptions, this one included, so each slot is
        // re-read and the callback held across its own call.
        if(!table->slots[i].handle) continue;
        JSValue payload=JS_UNDEFINED;
        if(!build(ctx,i,user,&payload)) continue;
        uint32_t handle=table->slots[i].handle;
        JSValue fn=JS_DupValue(ctx,table->slots[i].callback);
        JSValue result=JS_Call(ctx,fn,JS_UNDEFINED,1,(JSValueConst *)&payload);
        if(JS_IsException(result)) {
            JSValue error=JS_GetException(ctx);
            const char *text=JS_ToCString(ctx,error);
            ESP_LOGW(table->tag,"%s listener failed: %s",table->what,text?text:"?");
            if(text) JS_FreeCString(ctx,text);
            JS_FreeValue(ctx,error);
            if(table->close_on_throw && table->slots[i].handle==handle)
                pocket_api_sub_close(table,i);
        }
        JS_FreeValue(ctx,result);
        JS_FreeValue(ctx,payload);
        JS_FreeValue(ctx,fn);
    }
}

void pocket_api_sub_mark(pocket_sub_table_t *table, JSRuntime *rt,
                         JS_MarkFunc *mark) {
    for(int i=0;i<table->count;i++) JS_MarkValue(rt,table->slots[i].callback,mark);
}

// ------------------------------------------------------- async completions
//
// See the header for what a request number is and why its counter is never
// reset. A slot is claimed before the driver starts, so a driver that finishes
// before it has even returned its handle has somewhere to post to; the JS task
// reads that post on its next pump.

typedef struct {
    // Written by the JS task, read by driver tasks and ISRs.
    atomic_uint request;        // 0 marks a free slot
    // Written by the driver, read by the JS task.
    atomic_uint done;           // the request the driver finished, 0 for none
    atomic_int  status;
    bool        armed;          // a Promise exists to settle
    JSContext  *ctx;
    JSValue     resolve, reject;
    JSValue     cancel;         // JS_UNDEFINED when the call passed no token
    int64_t     deadline_us;
    const char *stop_code;      // NULL until the host asked the work to stop
    const pocket_promise_ops_t *ops;
    void       *user;
} pocket_promise_t;

static pocket_promise_t promises[POCKET_MAX_PROMISES];
static unsigned         promise_open;      // slots claimed, armed or not
// Never reset. See the header: this is what makes a request number unique for
// the life of the run, and an ended session's completions unreadable.
static uint32_t         promise_counter = 1;

static pocket_promise_t *promise_of(pocket_request_t request) {
    if(!request) return NULL;
    pocket_promise_t *p=&promises[request&(POCKET_MAX_PROMISES-1)];
    return atomic_load(&p->request)==request?p:NULL;
}

static void promise_release(pocket_promise_t *p) {
    if(p->ctx) {
        JS_FreeValue(p->ctx,p->resolve);
        JS_FreeValue(p->ctx,p->reject);
        JS_FreeValue(p->ctx,p->cancel);
    }
    p->resolve=p->reject=p->cancel=JS_UNDEFINED;
    p->ctx=NULL;
    p->armed=false;
    p->stop_code=NULL;
    const pocket_promise_ops_t *ops=p->ops;
    void                       *user=p->user;
    p->ops=NULL;
    p->user=NULL;
    atomic_store(&p->request,0);
    promise_open--;
    // Last, so the surface finds the slot already free.
    if(ops && ops->release) ops->release(user);
}

pocket_request_t pocket_api_promise_open(void) {
    for(unsigned i=0;i<POCKET_MAX_PROMISES;i++) {
        pocket_promise_t *p=&promises[i];
        if(atomic_load(&p->request)) continue;
        atomic_store(&p->done,0);
        atomic_store(&p->status,POCKET_STATUS_OK);
        p->armed=false;
        p->ctx=NULL;
        p->resolve=p->reject=p->cancel=JS_UNDEFINED;
        p->deadline_us=0;
        p->stop_code=NULL;
        p->ops=NULL;
        p->user=NULL;
        // The counter keeps the bits the slot index does not use, and skips 0 so
        // that no request number can collide with "none".
        uint32_t request=(promise_counter<<POCKET_PROMISE_SLOT_BITS)|i;
        promise_counter=(promise_counter+1)&
                        ((1u<<(32-POCKET_PROMISE_SLOT_BITS))-1u);
        if(!promise_counter) promise_counter=1;
        atomic_store(&p->request,request);
        promise_open++;
        return request;
    }
    return 0;
}

void pocket_api_promise_abandon(pocket_request_t request) {
    pocket_promise_t *p=promise_of(request);
    if(p) promise_release(p);
}

JSValue pocket_api_promise_arm(JSContext *ctx, pocket_request_t request,
                               const pocket_promise_ops_t *ops, void *user,
                               JSValue cancel, int64_t deadline_us) {
    pocket_promise_t *p=promise_of(request);
    if(!p) {
        JS_FreeValue(ctx,cancel);
        return JS_ThrowInternalError(ctx,"pocket: no such request");
    }
    JSValue funcs[2];
    JSValue promise=JS_NewPromiseCapability(ctx,funcs);
    if(JS_IsException(promise)) {
        // Nothing can settle a Promise that was not built, so the slot goes
        // back; the driver's completion then matches no request and is never
        // read. `ops` is not installed yet, so no release hook runs for a call
        // that never armed.
        JS_FreeValue(ctx,cancel);
        promise_release(p);
        return promise;
    }
    p->ctx=ctx;
    p->resolve=funcs[0];
    p->reject=funcs[1];
    p->cancel=cancel;
    p->deadline_us=deadline_us;
    p->ops=ops;
    p->user=user;
    p->armed=true;
    return promise;
}

void pocket_api_complete(pocket_request_t request, int32_t status) {
    pocket_promise_t *p=promise_of(request);
    if(!p) return;
    atomic_store(&p->status,status);
    // Written last: the pump reads the number first and only then trusts the
    // status beside it.
    atomic_store(&p->done,request);
}

static void promise_settle(pocket_promise_t *p, JSValue value, bool rejected) {
    JSContext *ctx=p->ctx;
    // A surface's settle() can only fail by running the guest heap out while
    // building its error. Settling with whatever QuickJS threw instead keeps the
    // rule that matters: a Promise handed to the app always settles.
    if(JS_IsException(value)) value=JS_GetException(ctx);
    JSValue done=JS_Call(ctx,rejected?p->reject:p->resolve,JS_UNDEFINED,1,
                         (JSValueConst *)&value);
    JS_FreeValue(ctx,done);
    JS_FreeValue(ctx,value);
    promise_release(p);
}

// Asks the driver to stop and remembers why. The Promise is not settled here;
// see pocket_promise_ops_t.stop.
static void promise_stop(pocket_promise_t *p, const char *code) {
    if(p->stop_code) return;
    p->stop_code=code;
    if(p->ops->stop) p->ops->stop(p->user,code);
}

void pocket_api_pump(void) {
    if(!promise_open) return;
    int64_t now=esp_timer_get_time();
    for(unsigned i=0;i<POCKET_MAX_PROMISES;i++) {
        pocket_promise_t *p=&promises[i];
        uint32_t request=atomic_load(&p->request);
        if(!request || !p->armed) continue;
        if(atomic_load(&p->done)==request) {
            bool    rejected=false;
            JSValue value=p->ops->settle(p->ctx,p->user,atomic_load(&p->status),
                                         p->stop_code,&rejected);
            promise_settle(p,value,rejected);
            continue;
        }
        if(p->stop_code) continue;    // already stopping; waiting for the driver
        if(!JS_IsUndefined(p->cancel) && pocket_api_cancel_requested(p->cancel))
            promise_stop(p,POCKET_ERR_CANCELLED);
        else if(now>p->deadline_us)
            promise_stop(p,POCKET_ERR_TIMEOUT);
    }
}

void pocket_api_reset(void) {
    for(unsigned i=0;i<POCKET_MAX_PROMISES;i++) {
        pocket_promise_t *p=&promises[i];
        if(!atomic_load(&p->request)) continue;
        // The realm is going away, so there is nobody left to settle to. The
        // work is asked to stop and then left alone: its completion carries a
        // request number this session will never read again, and the driver
        // holds no JS value to free.
        if(p->ops && p->ops->stop) p->ops->stop(p->user,POCKET_ERR_CLOSED);
        promise_release(p);
    }
}

// ---------------------------------------------------------- capabilities

static JSValue limits_object(JSContext *ctx, const pocket_limit_t *limits) {
    JSValue object=JS_NewObject(ctx);
    if(JS_IsException(object) || !limits) return object;
    for(;limits->kind!=POCKET_LIMIT_END;limits++) {
        JSValue value;
        switch(limits->kind) {
            case POCKET_LIMIT_TEXT: value=JS_NewString(ctx,limits->text?limits->text:""); break;
            case POCKET_LIMIT_FLAG: value=JS_NewBool(ctx,limits->number!=0); break;
            default:                value=JS_NewInt32(ctx,limits->number); break;
        }
        JS_SetPropertyStr(ctx,object,limits->name,value);
    }
    return object;
}

// An unknown name is answered, not refused: section 2 requires supported=false
// so that an app written against a later firmware still runs here.
static JSValue capability_object(JSContext *ctx, const char *name) {
    const pocket_capability_t *cap=lookup(name);
    bool        supported=false, available=false;
    const char *reason=POCKET_REASON_NOT_IMPLEMENTED;
    if(cap) {
        supported=cap->supported;
        available=cap->available;
        reason=cap->reason;
        if(cap->probe) cap->probe(cap,&available,&reason);
        // available is about a supported feature having the device, setting and
        // room to run right now; without the feature there is nothing to have.
        if(!supported) available=false;
    }
    JSValue object=JS_NewObject(ctx);
    if(JS_IsException(object)) return object;
    JS_SetPropertyStr(ctx,object,"name",JS_NewString(ctx,name));
    JS_SetPropertyStr(ctx,object,"supported",JS_NewBool(ctx,supported));
    JS_SetPropertyStr(ctx,object,"available",JS_NewBool(ctx,available));
    JS_SetPropertyStr(ctx,object,"reason",
                      reason?JS_NewString(ctx,reason):JS_NULL);
    JS_SetPropertyStr(ctx,object,"limits",
                      limits_object(ctx,cap?cap->limits:NULL));
    return object;
}

static JSValue js_capabilities_get(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    (void)this_val;
    if(argc<1 || !JS_IsString(argv[0]))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"capabilities.get",
                                "name must be a string",false,NULL);
    const char *name=JS_ToCString(ctx,argv[0]);
    if(!name) return JS_EXCEPTION;
    JSValue result=capability_object(ctx,name);
    JS_FreeCString(ctx,name);
    return result;
}

static JSValue js_capabilities_on_change(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
    (void)this_val;
    if(argc<1 || !JS_IsFunction(ctx,argv[0]))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,
                                "capabilities.onChange",
                                "listener must be a function",false,NULL);
    if(!state) return pocket_api_throw(ctx,POCKET_ERR_CLOSED,
                                       "capabilities.onChange",
                                       "the pocket API is not active",false,NULL);
    return pocket_api_sub_open(ctx,&state->subs,argv[0],"capabilities.onChange",
                               "too many subscriptions",NULL);
}

// Every subscriber sees the same capability, but each gets its own object: the
// listener before it may have kept, or changed, the one it was handed.
static bool capability_payload(JSContext *ctx, int slot, void *user,
                               JSValue *payload) {
    (void)slot;
    *payload=capability_object(ctx,(const char *)user);
    return true;
}

void pocket_api_capability_changed(const char *name) {
    if(!state || !name) return;
    pocket_api_sub_deliver(&state->subs,capability_payload,(void *)name);
}

// ---------------------------------------------------------------- device

static JSValue js_device_info(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    JSValue info=JS_NewObject(ctx);
    if(JS_IsException(info)) return info;
    // The app description carries the git describe output, or the build time
    // when the tree has no tag to describe.
    const esp_app_desc_t *desc=esp_app_get_description();
    JS_SetPropertyStr(ctx,info,"model",JS_NewString(ctx,"Cardputer ADV"));
    JS_SetPropertyStr(ctx,info,"firmware",
                      JS_NewString(ctx,desc?desc->version:"unknown"));
    JSValue display=JS_NewObject(ctx);
    JS_SetPropertyStr(ctx,display,"width",JS_NewInt32(ctx,LCD_W));
    JS_SetPropertyStr(ctx,display,"height",JS_NewInt32(ctx,LCD_H));
    JS_SetPropertyStr(ctx,info,"display",display);
    return info;
}

// ---------------------------------------------------------------- install

static void hub_finalizer(JSRuntime *rt, JSValueConst value) {
    pocket_state_t *st=JS_GetOpaque(value,hub_class);
    if(!st) return;
    JS_FreeValueRT(rt,st->error_proto);
    for(int i=0;i<POCKET_MAX_SUBSCRIPTIONS;i++)
        JS_FreeValueRT(rt,st->subscribers[i].callback);
    if(state==st) state=NULL;
    free(st);
}

// The hub holds the only strong references the host keeps into the realm, and a
// listener closing over pocket makes a cycle back to it. Marking lets the
// collector break that instead of holding the realm until it is torn down.
static void hub_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark) {
    pocket_state_t *st=JS_GetOpaque(value,hub_class);
    if(!st) return;
    JS_MarkValue(rt,st->error_proto,mark);
    pocket_api_sub_mark(&st->subs,rt,mark);
}

static const JSClassDef hub_class_def = {
    .class_name="PocketHub", .finalizer=hub_finalizer, .gc_mark=hub_mark,
};
static const JSClassDef token_class_def = {
    .class_name="PocketCancelToken", .finalizer=token_finalizer,
};

// Read-only, non-configurable, enumerable: section 2 asks for a public root a
// program cannot reshape, without freezing away namespaces added later.
static void define(JSContext *ctx, JSValueConst object,
                   const char *name, JSValue value) {
    JS_DefinePropertyValueStr(ctx,object,name,value,JS_PROP_ENUMERABLE);
}

// ------------------------------------------------------------ lazy namespaces
//
// See the header for why. The table is plain C data; the only thing on the
// guest heap before an app touches anything is one accessor per name.

typedef struct {
    const char         *name;       // static; not copied
    pocket_namespace_fn contribute;
    void               *user;
} pocket_lazy_t;

static pocket_lazy_t lazy[POCKET_MAX_LAZY];
static unsigned      lazy_count;

// The one getter behind every accessor. Its magic is the index of the first
// contributor for the name, and the rest are found by walking forward -- the
// two names with a second contributor register it later in the same session,
// and pocket.device's second is several installs away, so matching by name
// rather than by adjacency is what makes the order the registration order and
// not the file order.
static JSValue js_lazy_namespace(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv, int magic) {
    (void)argc; (void)argv;
    if(magic<0 || (unsigned)magic>=lazy_count) return JS_UNDEFINED;
    const char *name=lazy[magic].name;
    JSValue ns=JS_NewObject(ctx);
    if(JS_IsException(ns)) return ns;
    esp_err_t err=ESP_OK;
    for(unsigned i=(unsigned)magic;i<lazy_count && err==ESP_OK;i++)
        if(!strcmp(lazy[i].name,name))
            err=lazy[i].contribute(ctx,ns,lazy[i].user);
    if(err!=ESP_OK) {
        // Nothing half-built is ever installed: the partial object goes and the
        // accessor stays, so a read after the heap has moved builds it properly
        // rather than finding a namespace missing half its methods. retryable
        // says so, because on this board that is often true a second later.
        JS_FreeValue(ctx,ns);
        ESP_LOGW(TAG,"pocket.%s could not be built: %s",name,
                 esp_err_to_name(err));
        return pocket_api_throw(ctx,POCKET_ERR_OUT_OF_MEMORY,name,
                                "no memory to build this namespace",true,NULL);
    }
    // Replaces this accessor with the value it produced, in the descriptor
    // shape the property had when it was built eagerly: enumerable, and
    // neither writable nor configurable. The getter's own function object has
    // no referent afterwards and goes with the next collection, so a namespace
    // an app does touch costs what it always cost and nothing more.
    JS_DefinePropertyValueStr(ctx,this_val,name,JS_DupValue(ctx,ns),
                              JS_PROP_ENUMERABLE);
    return ns;
}

esp_err_t pocket_api_lazy(JSContext *ctx, const char *name,
                          pocket_namespace_fn contribute, void *user) {
    if(!name || !contribute) return ESP_ERR_INVALID_ARG;
    if(lazy_count>=POCKET_MAX_LAZY) {
        // Loud for the same reason the capability table is: the symptom of a
        // silent truncation here is a namespace that is simply absent, which
        // an app cannot tell from one this firmware does not implement.
        ESP_LOGE(TAG,"lazy table full at %u; pocket.%s will not exist",
                 POCKET_MAX_LAZY,name);
        return ESP_ERR_NO_MEM;
    }
    bool first=true;
    for(unsigned i=0;i<lazy_count;i++)
        if(!strcmp(lazy[i].name,name)) { first=false; break; }
    unsigned slot=lazy_count;
    lazy[slot]=(pocket_lazy_t){.name=name,.contribute=contribute,.user=user};
    lazy_count++;
    if(!first) return ESP_OK;   // the accessor is already on the root

    JSValue root=pocket_api_root(ctx);
    if(!JS_IsObject(root)) {
        // Installing pocket_api first is the caller's job; doing it silently
        // here would hide the ordering bug rather than report it.
        JS_FreeValue(ctx,root);
        lazy_count--;
        return ESP_ERR_INVALID_STATE;
    }
    // generic_magic rather than getter_magic, and not for want of trying the
    // latter: a getter is invoked as an ordinary zero-argument call with the
    // object as `this`, so the general entry point is both the honest type and
    // the one that needs no cast between incompatible function pointers.
    JSValue getter=JS_NewCFunctionMagic(ctx,js_lazy_namespace,name,0,
                                        JS_CFUNC_generic_magic,(int)slot);
    JSAtom atom=JS_NewAtom(ctx,name);
    // Configurable, unlike the property it becomes: replacing an accessor with
    // a value needs it, and the window in which it is configurable ends with
    // the first read.
    int ok=JS_DefinePropertyGetSet(ctx,root,atom,getter,JS_UNDEFINED,
                                   JS_PROP_ENUMERABLE|JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx,atom);
    JS_FreeValue(ctx,root);
    if(ok<0) { lazy_count--; return ESP_FAIL; }
    return ESP_OK;
}

static JSValue error_prototype(JSContext *ctx) {
    JSValue global=JS_GetGlobalObject(ctx);
    JSValue base=JS_GetPropertyStr(ctx,global,"Error");
    JSValue base_proto=JS_GetPropertyStr(ctx,base,"prototype");
    JSValue proto=JS_NewObjectProto(ctx,base_proto);
    if(!JS_IsException(proto))
        JS_SetPropertyStr(ctx,proto,"name",JS_NewString(ctx,"PocketError"));
    JS_FreeValue(ctx,base_proto);
    JS_FreeValue(ctx,base);
    JS_FreeValue(ctx,global);
    return proto;
}

// The three namespaces this file owns. Each is one object and one or two
// functions, which is little -- but it is little for every app, including the
// ones that never mention them.

static esp_err_t build_device(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    define(ctx,ns,"info",JS_NewCFunction(ctx,js_device_info,"info",0));
    return ESP_OK;
}

static esp_err_t build_cancel(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    define(ctx,ns,"source",JS_NewCFunction(ctx,js_cancel_source,"source",0));
    return ESP_OK;
}

static esp_err_t build_error_codes(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    for(unsigned i=0;i<sizeof(error_codes)/sizeof(error_codes[0]);i++)
        define(ctx,ns,error_codes[i],JS_NewString(ctx,error_codes[i]));
    return ESP_OK;
}

esp_err_t pocket_api_install(JSContext *ctx, void *user_data) {
    (void)user_data;
    // First: a start that failed part way through leaves contributors behind,
    // and the next session must not inherit them.
    lazy_count=0;
    JSRuntime *rt=JS_GetRuntime(ctx);
    JS_NewClassID(rt,&hub_class);
    JS_NewClassID(rt,&token_class);
    if(JS_NewClass(rt,hub_class,&hub_class_def)<0) return ESP_FAIL;
    if(JS_NewClass(rt,token_class,&token_class_def)<0) return ESP_FAIL;
    // A class with no prototype registered produces objects whose prototype is
    // null, and those have no toString: String(token), `${token}` and print(token)
    // all throw TypeError rather than saying something unhelpful. A debugging
    // line is not where an API should fail, so the token gets a plain object
    // prototype and stringifies as [object Object].
    JSValue hub_proto=JS_NewObject(ctx), token_proto=JS_NewObject(ctx);
    if(JS_IsException(hub_proto)||JS_IsException(token_proto)) {
        JS_FreeValue(ctx,hub_proto); JS_FreeValue(ctx,token_proto);
        return ESP_FAIL;
    }
    JS_SetClassProto(ctx,hub_class,hub_proto);
    JS_SetClassProto(ctx,token_class,token_proto);

    if(state) {
        // One realm at a time; a second would leave the first unreachable.
        ESP_LOGW(TAG,"pocket API already installed on another realm");
        return ESP_ERR_INVALID_STATE;
    }
    pocket_state_t *st=calloc(1,sizeof(*st));
    if(!st) return ESP_ERR_NO_MEM;
    st->ctx=ctx;
    st->error_proto=JS_UNDEFINED;
    for(int i=0;i<POCKET_MAX_SUBSCRIPTIONS;i++)
        st->subscribers[i].callback=JS_UNDEFINED;
    // The table is inside the hub, so the collector can see the listeners and a
    // listener that closes over pocket does not pin the realm. close_on_throw is
    // off here: onChange fires only when a capability moves, so a listener that
    // throws costs nothing to keep, and taking its subscription away over one
    // bad call would be a surprise the two polled surfaces do not have.
    st->subs=(pocket_sub_table_t){
        .slots=st->subscribers, .count=POCKET_MAX_SUBSCRIPTIONS, .ctx=ctx,
        .tag=TAG, .what="onChange", .close_on_throw=false,
    };

    JSValue hub=JS_NewObjectClass(ctx,hub_class);
    if(JS_IsException(hub)) { free(st); return ESP_FAIL; }
    JS_SetOpaque(hub,st);
    state=st;
    st->error_proto=error_prototype(ctx);

    JSValue root=JS_NewObject(ctx);
    if(JS_IsException(root)) {
        // Out of memory here used to be permanent rather than per-session: hub
        // would be defined onto an exception, its finalizer would never run,
        // and state stayed set, so every later app_start_test() answered
        // INVALID_STATE until the device was rebooted.
        JS_FreeValue(ctx,hub);
        JS_FreeValue(ctx,st->error_proto);
        state=NULL; free(st);
        return ESP_ERR_NO_MEM;
    }
    define(ctx,root,"apiVersion",JS_NewString(ctx,POCKET_API_VERSION));

    // Eager, and the only namespace that is. An app feature-tests before it
    // instantiates, so the thing it asks with must not itself instantiate
    // anything; see the header.
    JSValue capabilities=JS_NewObject(ctx);
    define(ctx,capabilities,"get",
           JS_NewCFunction(ctx,js_capabilities_get,"get",1));
    define(ctx,capabilities,"onChange",
           JS_NewCFunction(ctx,js_capabilities_on_change,"onChange",1));
    define(ctx,root,"capabilities",capabilities);

    // The hub is not part of the public shape; it hangs off the root only so
    // that the realm owns the state and the finalizer runs with it.
    JS_DefinePropertyValueStr(ctx,root,"__hub",hub,0);

    // Before the accessors: pocket_api_lazy() finds the root through
    // globalThis, so the root has to be reachable from it first.
    JSValue global=JS_GetGlobalObject(ctx);
    JS_DefinePropertyValueStr(ctx,global,"pocket",root,JS_PROP_ENUMERABLE);
    JS_FreeValue(ctx,global);

    // device is built lazily even though it is this file's own, because
    // pocket_app.c adds metrics to it and a namespace with two contributors is
    // exactly what the mechanism is for. errorCodes is seventeen strings most
    // apps never read.
    pocket_api_lazy(ctx,"device",build_device,NULL);
    pocket_api_lazy(ctx,"cancel",build_cancel,NULL);
    pocket_api_lazy(ctx,"errorCodes",build_error_codes,NULL);
    return ESP_OK;
}

JSValue pocket_api_root(JSContext *ctx) {
    JSValue global=JS_GetGlobalObject(ctx);
    JSValue root=JS_GetPropertyStr(ctx,global,"pocket");
    JS_FreeValue(ctx,global);
    return root;
}
