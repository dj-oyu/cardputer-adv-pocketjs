#include "pocket_api.h"
#include "board.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

// How many capabilities other native surfaces may publish on top of the
// declared set, and how many onChange subscriptions one realm may hold. Both
// are pointer-sized tables so the cost in .bss stays flat.
#define POCKET_MAX_REGISTERED    16
#define POCKET_MAX_SUBSCRIPTIONS 8

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

// Per-realm state. It lives in a hub object the pocket namespace holds, so the
// realm's finalizer is what ends its life: nothing here outlives the guest.
typedef struct {
    JSContext *ctx;
    JSValue    error_proto;
    JSValue    subscribers[POCKET_MAX_SUBSCRIPTIONS];
    uint32_t   handles[POCKET_MAX_SUBSCRIPTIONS];   // 0 marks a free slot
    uint32_t   next_handle;
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

esp_err_t pocket_api_register(const pocket_capability_t *capability) {
    if(!capability || !capability->name) return ESP_ERR_INVALID_ARG;
    for(unsigned i=0;i<override_count;i++) {
        if(!strcmp(overrides[i]->name,capability->name)) {
            overrides[i]=capability;
            return ESP_OK;
        }
    }
    if(override_count>=POCKET_MAX_REGISTERED) return ESP_ERR_NO_MEM;
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

// close() is bound to a slot and the handle that slot held when it was made, so
// a second call finds a mismatch and does nothing.
static JSValue js_subscription_close(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv, int magic,
                                     JSValueConst *func_data) {
    (void)this_val; (void)argc; (void)argv;
    uint32_t handle=0;
    if(JS_ToUint32(ctx,&handle,func_data[0])) return JS_EXCEPTION;
    if(state && magic>=0 && magic<POCKET_MAX_SUBSCRIPTIONS
       && state->handles[magic]==handle && handle!=0) {
        state->handles[magic]=0;
        JS_FreeValue(ctx,state->subscribers[magic]);
        state->subscribers[magic]=JS_UNDEFINED;
    }
    return JS_UNDEFINED;
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
    int slot=-1;
    for(int i=0;i<POCKET_MAX_SUBSCRIPTIONS;i++)
        if(state->handles[i]==0) { slot=i; break; }
    if(slot<0) return pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,
                                       "capabilities.onChange",
                                       "too many subscriptions",false,NULL);
    if(++state->next_handle==0) state->next_handle=1;
    state->handles[slot]=state->next_handle;
    state->subscribers[slot]=JS_DupValue(ctx,argv[0]);
    JSValue subscription=JS_NewObject(ctx);
    if(JS_IsException(subscription)) {
        state->handles[slot]=0;
        JS_FreeValue(ctx,state->subscribers[slot]);
        state->subscribers[slot]=JS_UNDEFINED;
        return subscription;
    }
    JSValue handle=JS_NewUint32(ctx,state->handles[slot]);
    JSValue close=JS_NewCFunctionData(ctx,js_subscription_close,0,slot,1,&handle);
    JS_FreeValue(ctx,handle);
    JS_SetPropertyStr(ctx,subscription,"close",close);
    return subscription;
}

void pocket_api_capability_changed(const char *name) {
    if(!state || !name) return;
    JSContext *ctx=state->ctx;
    for(int i=0;i<POCKET_MAX_SUBSCRIPTIONS;i++) {
        // A listener may close subscriptions, this one included, so each slot is
        // re-read and the callback held across its own call.
        if(state->handles[i]==0) continue;
        JSValue fn=JS_DupValue(ctx,state->subscribers[i]);
        JSValue capability=capability_object(ctx,name);
        JSValue result=JS_Call(ctx,fn,JS_UNDEFINED,1,(JSValueConst *)&capability);
        if(JS_IsException(result)) {
            JSValue error=JS_GetException(ctx);
            const char *text=JS_ToCString(ctx,error);
            ESP_LOGW(TAG,"onChange listener failed: %s",text?text:"?");
            if(text) JS_FreeCString(ctx,text);
            JS_FreeValue(ctx,error);
        }
        JS_FreeValue(ctx,result);
        JS_FreeValue(ctx,capability);
        JS_FreeValue(ctx,fn);
    }
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
        JS_FreeValueRT(rt,st->subscribers[i]);
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
    for(int i=0;i<POCKET_MAX_SUBSCRIPTIONS;i++)
        JS_MarkValue(rt,st->subscribers[i],mark);
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

esp_err_t pocket_api_install(JSContext *ctx, void *user_data) {
    (void)user_data;
    JSRuntime *rt=JS_GetRuntime(ctx);
    JS_NewClassID(rt,&hub_class);
    JS_NewClassID(rt,&token_class);
    if(JS_NewClass(rt,hub_class,&hub_class_def)<0) return ESP_FAIL;
    if(JS_NewClass(rt,token_class,&token_class_def)<0) return ESP_FAIL;

    if(state) {
        // One realm at a time; a second would leave the first unreachable.
        ESP_LOGW(TAG,"pocket API already installed on another realm");
        return ESP_ERR_INVALID_STATE;
    }
    pocket_state_t *st=calloc(1,sizeof(*st));
    if(!st) return ESP_ERR_NO_MEM;
    st->ctx=ctx;
    st->error_proto=JS_UNDEFINED;
    for(int i=0;i<POCKET_MAX_SUBSCRIPTIONS;i++) st->subscribers[i]=JS_UNDEFINED;

    JSValue hub=JS_NewObjectClass(ctx,hub_class);
    if(JS_IsException(hub)) { free(st); return ESP_FAIL; }
    JS_SetOpaque(hub,st);
    state=st;
    st->error_proto=error_prototype(ctx);

    JSValue root=JS_NewObject(ctx);
    define(ctx,root,"apiVersion",JS_NewString(ctx,POCKET_API_VERSION));

    JSValue device=JS_NewObject(ctx);
    define(ctx,device,"info",JS_NewCFunction(ctx,js_device_info,"info",0));
    define(ctx,root,"device",device);

    JSValue capabilities=JS_NewObject(ctx);
    define(ctx,capabilities,"get",
           JS_NewCFunction(ctx,js_capabilities_get,"get",1));
    define(ctx,capabilities,"onChange",
           JS_NewCFunction(ctx,js_capabilities_on_change,"onChange",1));
    define(ctx,root,"capabilities",capabilities);

    JSValue cancel=JS_NewObject(ctx);
    define(ctx,cancel,"source",
           JS_NewCFunction(ctx,js_cancel_source,"source",0));
    define(ctx,root,"cancel",cancel);

    JSValue codes=JS_NewObject(ctx);
    for(unsigned i=0;i<sizeof(error_codes)/sizeof(error_codes[0]);i++)
        define(ctx,codes,error_codes[i],JS_NewString(ctx,error_codes[i]));
    define(ctx,root,"errorCodes",codes);

    // The hub is not part of the public shape; it hangs off the root only so
    // that the realm owns the state and the finalizer runs with it.
    JS_DefinePropertyValueStr(ctx,root,"__hub",hub,0);

    JSValue global=JS_GetGlobalObject(ctx);
    JS_DefinePropertyValueStr(ctx,global,"pocket",root,JS_PROP_ENUMERABLE);
    JS_FreeValue(ctx,global);
    return ESP_OK;
}

JSValue pocket_api_root(JSContext *ctx) {
    JSValue global=JS_GetGlobalObject(ctx);
    JSValue root=JS_GetPropertyStr(ctx,global,"pocket");
    JS_FreeValue(ctx,global);
    return root;
}
