// The slice of main/kobako/kobako_api.c a single surface needs in order to be
// linked without the rest of the firmware: the capability table is a list, a
// KobakoError is an Error carrying `code`, and a lazy namespace is built at
// once and hung on globalThis under its own name.
//
// This exists so tools/test_kobako_text.c can drive the REAL kobako_text.c --
// the file with the session lifetime in it -- against the REAL QuickJS. The
// crash class this was written for (a listener that closes its own session)
// cannot be reached from textfield.c, because textfield.c has no session.
#include "kobako_api.h"
#include <string.h>

static const kobako_capability_t *caps[16];
static int ncaps;

JSValue kobako_api_root(JSContext *ctx) {
    JSValue global=JS_GetGlobalObject(ctx),root=JS_GetPropertyStr(ctx,global,"kobako");
    if(JS_IsObject(root)){JS_FreeValue(ctx,global);return root;}
    JS_FreeValue(ctx,root);return global;
}

esp_err_t kobako_api_register(const kobako_capability_t *capability) {
    for(int i=0;i<ncaps;i++)
        if(!strcmp(caps[i]->name,capability->name)) { caps[i]=capability; return ESP_OK; }
    if(ncaps==(int)(sizeof caps/sizeof caps[0])) return ESP_FAIL;
    caps[ncaps++]=capability;
    return ESP_OK;
}

const kobako_capability_t *host_capability(const char *name) {
    for(int i=0;i<ncaps;i++) if(!strcmp(caps[i]->name,name)) return caps[i];
    return NULL;
}
static void host_class_owners_forget(void);
void host_capabilities_clear(void) { ncaps=0; host_class_owners_forget(); }

// Use the production registry implementation; never drop a lazy class owner.
static kobako_class_registry_t class_registry;
bool kobako_api_class_ready(JSRuntime *rt,kobako_class_owner_t *owner,JSClassID *id) {
    return kobako_class_registry_ready(&class_registry,rt,owner,id);
}
void kobako_api_class_forget(kobako_class_owner_t *owner) {
    kobako_class_registry_forget(&class_registry,owner);
}
static void host_class_owners_forget(void) {
    kobako_class_registry_reset(&class_registry);
}

bool kobako_api_supported(const char *name) { return host_capability(name)!=NULL; }

void kobako_api_capability_changed(const char *name) { (void)name; }

JSValue kobako_api_error(JSContext *ctx, const char *code, const char *operation,
                         const char *message, bool retryable, const char *outcome) {
    JSValue e=JS_NewError(ctx);
    JS_SetPropertyStr(ctx,e,"code",JS_NewString(ctx,code));
    JS_SetPropertyStr(ctx,e,"operation",JS_NewString(ctx,operation?operation:""));
    JS_SetPropertyStr(ctx,e,"message",JS_NewString(ctx,message?message:code));
    JS_SetPropertyStr(ctx,e,"retryable",JS_NewBool(ctx,retryable));
    if(outcome) JS_SetPropertyStr(ctx,e,"outcome",JS_NewString(ctx,outcome));
    return e;
}

JSValue kobako_api_throw(JSContext *ctx, const char *code, const char *operation,
                         const char *message, bool retryable, const char *outcome) {
    return JS_Throw(ctx,kobako_api_error(ctx,code,operation,message,retryable,outcome));
}

// Eager here, unlike the firmware: the test wants the namespace to exist so it
// can call into it, and nothing in this harness measures the guest's heap.
esp_err_t kobako_api_lazy(JSContext *ctx, const char *name,
                          kobako_namespace_fn contribute, void *user) {
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
