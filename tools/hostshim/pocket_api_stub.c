// The slice of main/pocket/pocket_api.c a single surface needs in order to be
// linked without the rest of the firmware: the capability table is a list, a
// PocketError is an Error carrying `code`, and a lazy namespace is built at
// once and hung on globalThis under its own name.
//
// This exists so tools/test_pocket_text.c can drive the REAL pocket_text.c --
// the file with the session lifetime in it -- against the REAL QuickJS. The
// crash class this was written for (a listener that closes its own session)
// cannot be reached from textfield.c, because textfield.c has no session.
#include "pocket_api.h"
#include <string.h>

static const pocket_capability_t *caps[16];
static int ncaps;

esp_err_t pocket_api_register(const pocket_capability_t *capability) {
    for(int i=0;i<ncaps;i++)
        if(!strcmp(caps[i]->name,capability->name)) { caps[i]=capability; return ESP_OK; }
    if(ncaps==(int)(sizeof caps/sizeof caps[0])) return ESP_FAIL;
    caps[ncaps++]=capability;
    return ESP_OK;
}

const pocket_capability_t *host_capability(const char *name) {
    for(int i=0;i<ncaps;i++) if(!strcmp(caps[i]->name,name)) return caps[i];
    return NULL;
}
static void host_class_owners_forget(void);
void host_capabilities_clear(void) { ncaps=0; host_class_owners_forget(); }

// The firmware's copy lives in pocket_api.c and remembers every owner, so that
// the end of a session can clear them: a new runtime can land on the address a
// freed one had, and a bare pointer compare then claims a class is registered
// in a table that has never seen it. This test builds and frees runtimes in a
// loop, which is that case exactly, so it keeps the same bookkeeping -- the
// forgetting hangs off host_capabilities_clear(), the stub's session end.
static JSRuntime **class_owners[16];
static unsigned class_owner_count;

bool pocket_api_class_ready(JSRuntime *rt, JSRuntime **owner, JSClassID *id) {
    if(*owner==rt) return true;
    *id=0;
    *owner=rt;
    for(unsigned i=0;i<class_owner_count;i++) if(class_owners[i]==owner) return false;
    if(class_owner_count<sizeof(class_owners)/sizeof(class_owners[0]))
        class_owners[class_owner_count++]=owner;
    return false;
}

static void host_class_owners_forget(void) {
    for(unsigned i=0;i<class_owner_count;i++) *class_owners[i]=NULL;
}

bool pocket_api_supported(const char *name) { return host_capability(name)!=NULL; }

void pocket_api_capability_changed(const char *name) { (void)name; }

JSValue pocket_api_error(JSContext *ctx, const char *code, const char *operation,
                         const char *message, bool retryable, const char *outcome) {
    JSValue e=JS_NewError(ctx);
    JS_SetPropertyStr(ctx,e,"code",JS_NewString(ctx,code));
    JS_SetPropertyStr(ctx,e,"operation",JS_NewString(ctx,operation?operation:""));
    JS_SetPropertyStr(ctx,e,"message",JS_NewString(ctx,message?message:code));
    JS_SetPropertyStr(ctx,e,"retryable",JS_NewBool(ctx,retryable));
    if(outcome) JS_SetPropertyStr(ctx,e,"outcome",JS_NewString(ctx,outcome));
    return e;
}

JSValue pocket_api_throw(JSContext *ctx, const char *code, const char *operation,
                         const char *message, bool retryable, const char *outcome) {
    return JS_Throw(ctx,pocket_api_error(ctx,code,operation,message,retryable,outcome));
}

// Eager here, unlike the firmware: the test wants the namespace to exist so it
// can call into it, and nothing in this harness measures the guest's heap.
esp_err_t pocket_api_lazy(JSContext *ctx, const char *name,
                          pocket_namespace_fn contribute, void *user) {
    JSValue global=JS_GetGlobalObject(ctx);
    JSValue ns=JS_GetPropertyStr(ctx,global,name);
    if(!JS_IsObject(ns)) {
        JS_FreeValue(ctx,ns);
        ns=JS_NewObject(ctx);
        JS_SetPropertyStr(ctx,global,name,JS_DupValue(ctx,ns));
    }
    esp_err_t err=contribute(ctx,ns,user);
    JS_FreeValue(ctx,ns);
    JS_FreeValue(ctx,global);
    return err;
}
