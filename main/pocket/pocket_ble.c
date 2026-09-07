#include "pocket_ble.h"
#include "pocket_api.h"

// Section 12's surface, present as names and absent as behaviour.
//
// Section 2: "supported=falseの機能も名前空間／メソッドを持ち、呼出しは
// UNSUPPORTEDで失敗する". Before this file, `ble.central` answered
// capabilities.get() honestly -- pocket_api.c's builtins table has carried the
// entry since stage A -- but `pocket.ble` did not exist, so an app that skipped
// the feature test got a TypeError about reading `scan` of undefined. That is
// the one failure section 2 is written to prevent, because a TypeError is
// indistinguishable from the app's own bug.
//
// The capability entries are deliberately NOT re-registered here. They already
// say supported=false with NOT_IMPLEMENTED and no limits, which is exactly what
// this build can honestly claim; a second registration would be the same two
// rows written twice. Nor are section 12's proposed ceilings published as
// limits: this file enforces none of them, and limits is for what the code
// actually enforces.

// The operation names section 12 gives its methods, used as the PocketError's
// `operation` so a rejection says which call was refused.
static const char *const ble_operations[] = {
    "ble.scan", "ble.connect", "ble.peripheral.open",
};

static JSValue js_unsupported(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int magic) {
    (void)this_val; (void)argc; (void)argv;
    // A rejected Promise rather than a throw: every method in section 12
    // returns one, and section 4 puts the refusal inside it so that a caller
    // written as `await pocket.ble.scan(...)` catches it in the place it wrote
    // the catch. retryable=false -- no amount of waiting adds a stack to a
    // firmware that was linked without one -- and NOT_APPLIED because the
    // radio was never touched.
    return pocket_api_reject(ctx, POCKET_ERR_UNSUPPORTED, ble_operations[magic],
                             "BLE is not built into this firmware", false,
                             POCKET_OUTCOME_NOT_APPLIED);
}

// Magic rather than pocket_av.c's JS_NewCFunctionData: the operation strings
// are compile-time constants here, so an index into a static table costs
// nothing on the guest heap where a captured JSValue per method would cost
// three closures. (pocket_av.c takes data because add_unsupported() is called
// with names built by its caller.)
static void define_unsupported(JSContext *ctx, JSValue parent,
                               const char *method, int operation) {
    JS_DefinePropertyValueStr(ctx, parent, method,
        JS_NewCFunctionMagic(ctx, js_unsupported, method, 2,
                             JS_CFUNC_generic_magic, operation),
        JS_PROP_ENUMERABLE);
}

static esp_err_t build_ble(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    define_unsupported(ctx, (JSValue)ns, "scan", 0);
    define_unsupported(ctx, (JSValue)ns, "connect", 1);

    // Section 12 names peripheral as a later stage. It is here for the same
    // reason central is: `ble.peripheral` is already a capability name, so the
    // method it names has to refuse rather than be missing.
    JSValue peripheral = JS_NewObject(ctx);
    if (JS_IsException(peripheral)) return ESP_ERR_NO_MEM;
    define_unsupported(ctx, peripheral, "open", 2);
    JS_DefinePropertyValueStr(ctx, (JSValue)ns, "peripheral", peripheral,
                              JS_PROP_ENUMERABLE);
    return ESP_OK;
}

esp_err_t pocket_ble_install(JSContext *ctx, void *user_data) {
    (void)user_data;
    // Lazy like every other namespace: three functions and two objects is
    // little, but an app that never mentions BLE should pay none of it. On this
    // board that is not a style point -- guest heap is what runs out.
    return pocket_api_lazy(ctx, "ble", build_ble, NULL);
}
