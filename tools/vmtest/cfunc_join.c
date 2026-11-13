// Host test of the "join" mark on native functions (quickjs.c, Pocket;
// docs/perf/two-core-pipeline.md P2a). The firmware will hang "wait for the
// display task" on the join hook, so what this checks is completeness and
// precision: every native function the firmware creates calls the hook before
// it runs -- including ones created at run time by a marked call and getters
// from a function list -- and nothing the engine creates for itself does.
//
// Built and run by tools/vmtest/cfunc_join.sh.
#include <stdio.h>
#include <string.h>

#include "quickjs.h"

static int hooks;
static int failures;

static void hook(JSContext *ctx)
{
    (void)ctx;
    hooks++;
}

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL %s:%d ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } } while (0)

static JSValue fn_plain(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewInt32(ctx, 1);
}

static JSValue fn_data(JSContext *ctx, JSValueConst this_val, int argc,
                       JSValueConst *argv, int magic, JSValueConst *data)
{
    (void)this_val; (void)argc; (void)argv; (void)magic;
    return JS_DupValue(ctx, data[0]);
}

// A surface method that hands out a fresh native function, as a cancel token
// or a subscription's close() does.
static JSValue fn_maker(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSValue one = JS_NewInt32(ctx, 7);
    return JS_NewCFunctionData(ctx, fn_data, 0, 0, 1, &one);
}

static JSValue fn_throw(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "boom");
}

// A surface method that runs a guest callback, as Kasane's scene build does.
static JSValue fn_call(JSContext *ctx, JSValueConst this_val, int argc,
                       JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_UNDEFINED;
    JSValue r = JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);
    if (JS_IsException(r))
        return r;
    JS_FreeValue(ctx, r);
    // and creates a function of its own after the callback: still marked
    JSValue one = JS_NewInt32(ctx, 9);
    return JS_NewCFunctionData(ctx, fn_data, 0, 0, 1, &one);
}

static JSValue fn_get(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    return JS_NewInt32(ctx, 3);
}

static const JSCFunctionListEntry inner[] = {
    JS_CFUNC_DEF("deep", 0, fn_plain),
};

static const JSCFunctionListEntry list[] = {
    JS_CFUNC_DEF("listed", 0, fn_plain),
    JS_CGETSET_DEF("prop", fn_get, NULL),
    JS_OBJECT_DEF("nested", inner, 1, JS_PROP_CONFIGURABLE),
};

// The same shapes registered with no mark: an engine-side list.
static const JSCFunctionListEntry engine_list[] = {
    JS_CFUNC_DEF("listed", 0, fn_plain),
    JS_CGETSET_DEF("prop", fn_get, NULL),
};

static int run(JSContext *ctx, const char *src)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<t>", JS_EVAL_TYPE_GLOBAL);
    int r = -999;
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        JS_FreeValue(ctx, e);
        r = -1;
    } else {
        JS_ToInt32(ctx, &r, v);
    }
    JS_FreeValue(ctx, v);
    // drain jobs (Promise.finally etc.)
    JSContext *c;
    while (JS_ExecutePendingJob(JS_GetRuntime(ctx), &c) > 0) {
    }
    return r;
}

static void set_fn(JSContext *ctx, JSValue obj, const char *name, JSValue f)
{
    JS_SetPropertyStr(ctx, obj, name, f);
}

int main(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    JS_SetCFunctionJoinHook(rt, hook);
    JSValue g = JS_GetGlobalObject(ctx);

    // Created before marking: not marked.
    JSValue early = JS_NewCFunction(ctx, fn_plain, "early", 0);
    CHECK(JS_GetCFunctionJoin(early) == 0, "early marked");
    set_fn(ctx, g, "early", early);

    JS_MarkCFunctions(rt, true);
    JSValue pocket = JS_NewObject(ctx);
    set_fn(ctx, pocket, "plain", JS_NewCFunction(ctx, fn_plain, "plain", 0));
    JSValue d = JS_NewInt32(ctx, 5);
    set_fn(ctx, pocket, "data", JS_NewCFunctionData(ctx, fn_data, 0, 0, 1, &d));
    set_fn(ctx, pocket, "maker", JS_NewCFunction(ctx, fn_maker, "maker", 0));
    set_fn(ctx, pocket, "thrower", JS_NewCFunction(ctx, fn_throw, "thrower", 0));
    set_fn(ctx, pocket, "callIt", JS_NewCFunction(ctx, fn_call, "callIt", 1));
    JSValue optout = JS_NewCFunction(ctx, fn_plain, "optout", 0);
    CHECK(JS_SetCFunctionJoin(optout, false), "opt-out refused");
    set_fn(ctx, pocket, "optout", optout);
    JS_SetPropertyFunctionList(ctx, pocket, list, 3);
    // A span with the count zeroed inside a marked one: unmarked, list too.
    JSCFunctionMark saved = JS_SetCFunctionMark(rt, false);
    set_fn(ctx, pocket, "span", JS_NewCFunction(ctx, fn_plain, "span", 0));
    static const JSCFunctionListEntry span_list[] = {
        JS_CFUNC_DEF("spanListed", 0, fn_plain),
    };
    JS_SetPropertyFunctionList(ctx, pocket, span_list, 1);
    CHECK(JS_GetCFunctionMark(rt) == 0, "span not off");
    JS_RestoreCFunctionMark(rt, saved);
    CHECK(JS_GetCFunctionMark(rt) == 1, "span restore");
    // data_len gave its top bit to the flag
    JSValue many[128] = {0};
    for (int i = 0; i < 128; i++) many[i] = JS_UNDEFINED;
    JSValue big = JS_NewCFunctionData(ctx, fn_data, 0, 0, 128, many);
    CHECK(JS_IsException(big), "data_len 128 accepted");
    JS_FreeValue(ctx, JS_GetException(ctx));
    JS_MarkCFunctions(rt, false);
    set_fn(ctx, g, "pocket", pocket);
    JSValue engine = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, engine, engine_list, 2);
    set_fn(ctx, g, "engine", engine);

    CHECK(JS_GetCFunctionJoin(g) == -1, "plain object reported as native");

    struct { const char *src; int want_result; int want_hooks; } cases[] = {
        // Builtins first touched, and engine functions made, by guest JS that
        // a marked call runs: unmarked. callIt + its product = 2 hooks.
        { "pocket.callIt(() => { 'a'.padStart(3); [1].findLast(x => x);"
          " Promise.resolve(1).finally(() => 0); new Map(); })()", 9, 2 },
        { "'a'.padStart(3).length + [1].findLast(x => x) + new Map().size", 4, 0 },
        { "early()", 1, 0 },
        { "pocket.plain()", 1, 1 },
        { "pocket.data()", 5, 1 },
        { "pocket.listed()", 1, 1 },
        { "pocket.prop", 3, 1 },
        { "pocket.optout()", 1, 0 },
        { "pocket.nested.deep()", 1, 1 },
        { "pocket.span() + pocket.spanListed()", 2, 0 },
        { "Object.keys(pocket).length", 7, 0 },
        { "engine.listed() + engine.prop", 4, 0 },
        // a function made by a marked call is marked: maker + its product
        { "pocket.maker()()", 7, 2 },
        // an exception still lowers the mark (checked below)
        { "try { pocket.thrower() } catch (e) {} 0", 0, 1 },
        // through call/apply/bind and as a callback of a builtin
        { "pocket.plain.call(null)", 1, 1 },
        { "pocket.plain.bind(null)()", 1, 1 },
        { "[1,2].map(pocket.plain).length", 2, 2 },
        // the engine's own natives, also ones it makes at run time
        { "Math.max(1, 2) + [3].push(4) + JSON.stringify({a:1}).length", 11, 0 },
        { "Promise.resolve(1).finally(() => 0); Promise.all([1, 2]); 0", 0, 0 },
        { "[...'ab'].length + [...new Map([[1, 2]])].length", 3, 0 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        hooks = 0;
        int r = run(ctx, cases[i].src);
        CHECK(r == cases[i].want_result, "%s: result %d want %d", cases[i].src, r,
              cases[i].want_result);
        CHECK(hooks == cases[i].want_hooks, "%s: hooks %d want %d", cases[i].src,
              hooks, cases[i].want_hooks);
    }

    // The mark count is back to zero after all of that.
    JSValue late = JS_NewCFunction(ctx, fn_plain, "late", 0);
    CHECK(JS_GetCFunctionJoin(late) == 0, "mark count leaked");
    JS_FreeValue(ctx, late);

    JS_FreeValue(ctx, g);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    printf(failures ? "cfunc_join: %d failed\n" : "cfunc_join: ok\n", failures);
    return failures != 0;
}
