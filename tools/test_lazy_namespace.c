// What pocket_api.c's lazy namespaces do, run on the host against the same
// quickjs-ng the firmware links.
//
// The mechanism is small but its observable edges are not obvious from reading
// the API: whether an untouched namespace still enumerates, whether the
// accessor really becomes a plain property, what a second contributor sees, and
// what an app gets when a build refuses. None of that can be checked on the
// device without flashing, and this file is the answer -- js_lazy_namespace()
// and pocket_api_lazy() below are transcribed from main/pocket/pocket_api.c, minus the
// ESP logging. They are a copy, so a change there needs the same change here;
// the alternative was no coverage of the one part of the refactor that is a
// QuickJS semantics question rather than a C one.
//
//   wsl -e bash -lc "cd tools && ./build_lazy_test.sh && ./test_lazy_namespace"

#include "quickjs.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define POCKET_MAX_LAZY 28

typedef int (*contribute_fn)(JSContext *ctx, JSValueConst ns, void *user);

typedef struct {
    const char   *name;
    contribute_fn contribute;
    void         *user;
} lazy_t;

static lazy_t   lazy[POCKET_MAX_LAZY];
static unsigned lazy_count;

static JSValue pocket_root(JSContext *ctx) {
    JSValue global=JS_GetGlobalObject(ctx);
    JSValue root=JS_GetPropertyStr(ctx,global,"pocket");
    JS_FreeValue(ctx,global);
    return root;
}

// The firmware throws a PocketError here; the shape that matters to this test
// is the code and the operation, so a plain Error carrying both is enough.
static JSValue throw_oom(JSContext *ctx, const char *name) {
    JSValue error=JS_NewError(ctx);
    JS_SetPropertyStr(ctx,error,"code",JS_NewString(ctx,"OUT_OF_MEMORY"));
    JS_SetPropertyStr(ctx,error,"operation",JS_NewString(ctx,name));
    return JS_Throw(ctx,error);
}

static JSValue js_lazy_namespace(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv, int magic) {
    (void)argc; (void)argv;
    if(magic<0 || (unsigned)magic>=lazy_count) return JS_UNDEFINED;
    const char *name=lazy[magic].name;
    JSValue ns=JS_NewObject(ctx);
    if(JS_IsException(ns)) return ns;
    int err=0;
    for(unsigned i=(unsigned)magic;i<lazy_count && err==0;i++)
        if(!strcmp(lazy[i].name,name))
            err=lazy[i].contribute(ctx,ns,lazy[i].user);
    if(err) {
        JS_FreeValue(ctx,ns);
        return throw_oom(ctx,name);
    }
    JS_DefinePropertyValueStr(ctx,this_val,name,JS_DupValue(ctx,ns),
                              JS_PROP_ENUMERABLE);
    return ns;
}

static int lazy_register(JSContext *ctx, const char *name,
                         contribute_fn contribute, void *user) {
    if(lazy_count>=POCKET_MAX_LAZY) return -1;
    bool first=true;
    for(unsigned i=0;i<lazy_count;i++)
        if(!strcmp(lazy[i].name,name)) { first=false; break; }
    unsigned slot=lazy_count;
    lazy[slot]=(lazy_t){.name=name,.contribute=contribute,.user=user};
    lazy_count++;
    if(!first) return 0;

    JSValue root=pocket_root(ctx);
    if(!JS_IsObject(root)) { JS_FreeValue(ctx,root); lazy_count--; return -1; }
    JSValue getter=JS_NewCFunctionMagic(ctx,js_lazy_namespace,name,0,
                                        JS_CFUNC_generic_magic,(int)slot);
    JSAtom atom=JS_NewAtom(ctx,name);
    int ok=JS_DefinePropertyGetSet(ctx,root,atom,getter,JS_UNDEFINED,
                                   JS_PROP_ENUMERABLE|JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx,atom);
    JS_FreeValue(ctx,root);
    return ok<0?-1:0;
}

// ------------------------------------------------------------- contributors

static int storage_calls, pet_hub_calls, pet_assets_calls, flaky_calls;
static bool flaky_refuses = true;

static JSValue js_noop(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    (void)ctx; (void)t; (void)c; (void)v;
    return JS_UNDEFINED;
}

static int build_storage(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    storage_calls++;
    JS_DefinePropertyValueStr(ctx,ns,"get",
        JS_NewCFunction(ctx,js_noop,"get",2),JS_PROP_ENUMERABLE);
    return 0;
}

static int build_pet_hub(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    pet_hub_calls++;
    JS_DefinePropertyValueStr(ctx,ns,"select",
        JS_NewCFunction(ctx,js_noop,"select",1),JS_PROP_ENUMERABLE);
    return 0;
}

static int build_pet_assets(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    pet_assets_calls++;
    // Proves the second contributor sees the first's work.
    JSValue first=JS_GetPropertyStr(ctx,ns,"select");
    assert(JS_IsFunction(ctx,first));
    JS_FreeValue(ctx,first);
    JS_DefinePropertyValueStr(ctx,ns,"show",
        JS_NewCFunction(ctx,js_noop,"show",2),JS_PROP_ENUMERABLE);
    return 0;
}

static int build_flaky(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    flaky_calls++;
    // Half-builds before refusing, which is what a real surface running out of
    // room part way through would do.
    JS_DefinePropertyValueStr(ctx,ns,"half",JS_NewInt32(ctx,1),JS_PROP_ENUMERABLE);
    return flaky_refuses?-1:0;
}

// ---------------------------------------------------------------- the checks

static int failures;

static void check(bool ok, const char *what) {
    printf("%-58s %s\n",what,ok?"ok":"FAILED");
    if(!ok) failures++;
}

// Evaluates `source` and returns the result as a C string the caller frees with
// free_text(). A thrown exception comes back as "throw:" plus its message.
static char text_buffer[512];

static const char *eval_text(JSContext *ctx, const char *source) {
    JSValue v=JS_Eval(ctx,source,strlen(source),"<test>",JS_EVAL_TYPE_GLOBAL);
    JSValue shown=v;
    const char *prefix="";
    if(JS_IsException(v)) { shown=JS_GetException(ctx); prefix="throw:"; }
    const char *s=JS_ToCString(ctx,shown);
    snprintf(text_buffer,sizeof(text_buffer),"%s%s",prefix,s?s:"?");
    if(s) JS_FreeCString(ctx,s);
    JS_FreeValue(ctx,shown);
    if(!JS_IsException(v)) { /* shown == v, already freed */ }
    return text_buffer;
}

static bool eval_true(JSContext *ctx, const char *source) {
    return !strcmp(eval_text(ctx,source),"true");
}

int main(void) {
    JSRuntime *rt=JS_NewRuntime();
    JSContext *ctx=JS_NewContext(rt);

    JSValue root=JS_NewObject(ctx);
    JS_SetPropertyStr(ctx,root,"apiVersion",JS_NewString(ctx,"0.1.0"));
    JSValue global=JS_GetGlobalObject(ctx);
    JS_DefinePropertyValueStr(ctx,global,"pocket",root,JS_PROP_ENUMERABLE);
    JS_FreeValue(ctx,global);

    lazy_register(ctx,"storage",build_storage,NULL);
    lazy_register(ctx,"pet",build_pet_hub,NULL);
    lazy_register(ctx,"pet",build_pet_assets,NULL);   // second contributor
    lazy_register(ctx,"flaky",build_flaky,NULL);

    // 1. An untouched namespace is still there to be found.
    check(storage_calls==0,"registering builds nothing");
    check(eval_true(ctx,"'storage' in pocket"),"'in' finds an unbuilt namespace");
    check(eval_true(ctx,"Object.keys(pocket).indexOf('storage')>=0"),
          "Object.keys lists an unbuilt namespace");
    check(eval_true(ctx,
          "(function(){var n=0;for(var k in pocket)n++;return n===4})()"),
          "for-in sees all four names, built or not");
    check(storage_calls==0,"none of that built anything");

    // 2. The first read builds it, once.
    check(eval_true(ctx,"typeof pocket.storage.get==='function'"),
          "the first read builds the namespace");
    check(storage_calls==1,"the contributor ran once");
    check(eval_true(ctx,"pocket.storage===pocket.storage"),
          "a second read is the same object");
    check(storage_calls==1,"the contributor did not run again");

    // 3. And it now looks exactly like an eagerly built property.
    check(eval_true(ctx,
          "(function(d){return d.value!==undefined&&d.get===undefined&&"
          "d.enumerable===true&&d.writable===false&&d.configurable===false})"
          "(Object.getOwnPropertyDescriptor(pocket,'storage'))"),
          "the accessor became an enumerable, frozen data property");

    // 4. Two contributors, in registration order, on one object.
    check(eval_true(ctx,
          "typeof pocket.pet.select==='function'&&typeof pocket.pet.show==='function'"),
          "both contributors filled the same namespace");
    check(pet_hub_calls==1&&pet_assets_calls==1,"each contributor ran once");

    // 5. A refusal reaches the app as an error, leaves nothing half-built, and
    //    does not poison the property.
    const char *thrown=eval_text(ctx,
        "(function(){try{pocket.flaky;return 'no throw';}"
        "catch(e){return e.code+'/'+e.operation;}})()");
    check(!strcmp(thrown,"OUT_OF_MEMORY/flaky"),
          "a contributor's refusal throws OUT_OF_MEMORY for that namespace");
    check(eval_true(ctx,
          "Object.getOwnPropertyDescriptor(pocket,'flaky').get!==undefined"),
          "the accessor survives a refusal");
    check(eval_true(ctx,"Object.keys(pocket).indexOf('flaky')>=0"),
          "a refused namespace still enumerates");
    flaky_refuses=false;
    check(eval_true(ctx,"pocket.flaky.half===1"),"a later read succeeds");
    check(flaky_calls==2,"the retry re-ran the contributor from scratch");

    // 6. Whole-object operations do not trip over what is left.
    check(eval_true(ctx,"typeof JSON.stringify(pocket)==='string'"),
          "JSON.stringify(pocket) builds the rest and does not throw");

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    printf("\n%s\n",failures?"FAILED":"LAZY_NAMESPACE_OK");
    return failures?1:0;
}
