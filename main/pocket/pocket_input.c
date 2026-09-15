#include "pocket_input.h"
#include "pocket_api.h"
#include "esp_timer.h"
#include <string.h>
#define UI_ACTION_SUBS 4
// The pad bits pocketjs_ui_input_t.buttons carries, from
// .cache/pocketjs/contracts/generated/pocket_spec.h.
#define BTN_UP     0x0010u
#define BTN_RIGHT  0x0020u
#define BTN_DOWN   0x0040u
#define BTN_LEFT   0x0080u
#define BTN_CIRCLE 0x2000u
#define BTN_CROSS  0x4000u

#define REPEAT_DELAY_US  400000
#define REPEAT_PERIOD_US 120000

// ----------------------------------------------------------------- actions

static const struct { const char *name; uint32_t bit; } ACTIONS[] = {
    {"left",   BTN_LEFT},   {"right", BTN_RIGHT},
    {"up",     BTN_UP},     {"down",  BTN_DOWN},
    {"accept", BTN_CROSS},  {"back",  BTN_CIRCLE},
};
#define ACTION_COUNT (int)(sizeof(ACTIONS)/sizeof(ACTIONS[0]))

static JSValue bad(JSContext *ctx,const char *op,const char *what){
    return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,what,false,NULL);
}
// ------------------------------------------------------------ pocket.input

static pocket_sub_slot_t  action_slots[UI_ACTION_SUBS];
static pocket_sub_table_t action_table = {
    .slots=action_slots, .count=UI_ACTION_SUBS,
    .tag="pocket.ui", .what="onAction",
    // A listener throwing on every press would fill the log and keep costing a
    // call; the app keeps its other subscriptions. Same reasoning as imu.watch.
    .close_on_throw=true,
};
static uint32_t held_mask;
static int64_t  repeat_at[ACTION_COUNT];

typedef struct { int action; const char *phase; int64_t now; } action_event_t;

static bool action_payload(JSContext *ctx, int slot, void *user, JSValue *payload) {
    const action_event_t *e=user;
    (void)slot;
    JSValue object=JS_NewObject(ctx);
    if(JS_IsException(object)) return false;
    JS_SetPropertyStr(ctx,object,"action",
                      JS_NewString(ctx,ACTIONS[e->action].name));
    JS_SetPropertyStr(ctx,object,"phase",JS_NewString(ctx,e->phase));
    JS_SetPropertyStr(ctx,object,"timeMs",JS_NewFloat64(ctx,e->now/1000.0));
    *payload=object;
    return true;
}

static JSValue js_on_action(JSContext *ctx, JSValueConst self,
                            int argc, JSValueConst *argv) {
    (void)self;
    if(argc<1 || !JS_IsFunction(ctx,argv[0]))
        return bad(ctx,"input.onAction","listener must be a function");
    return pocket_api_sub_open(ctx,&action_table,argv[0],"input.onAction",
                               "too many input subscriptions",NULL);
}

static JSValue js_held(JSContext *ctx, JSValueConst self,
                       int argc, JSValueConst *argv) {
    (void)self;
    if(argc<1 || !JS_IsString(argv[0]))
        return bad(ctx,"input.held","held(action) takes an action name");
    const char *name=JS_ToCString(ctx,argv[0]);
    if(!name) return JS_EXCEPTION;
    for(int i=0;i<ACTION_COUNT;i++) {
        if(strcmp(name,ACTIONS[i].name)) continue;
        JS_FreeCString(ctx,name);
        return JS_NewBool(ctx,(held_mask&ACTIONS[i].bit)!=0);
    }
    JS_FreeCString(ctx,name);
    return bad(ctx,"input.held","no such action");
}

// Section 6 gives onKey a key/code/modifiers shape, and this host has no channel
// for it: main.c's keymap consumes the keyboard for the shell and hands a running
// app a pad mask alone. Section 2 says an unimplemented feature keeps its name
// and fails with UNSUPPORTED, which is a better answer than a subscription that
// silently never fires.
static JSValue js_on_key(JSContext *ctx, JSValueConst self,
                         int argc, JSValueConst *argv) {
    (void)self; (void)argc; (void)argv;
    return pocket_api_throw(ctx,POCKET_ERR_UNSUPPORTED,"input.onKey",
                            "this host delivers actions, not key events",
                            false,NULL);
}

// input.text used to be a second UNSUPPORTED stub here, on the argument that
// only a screen declaring takes_text gets the IME. That argument was wrong
// about where the field has to live, not about the machinery: pocket_text.c
// puts the field in the HOST and composites it over the guest's own frame, and
// main.c hands it the keyboard for as long as it is open. It contributes to
// this same namespace and defines input.text itself.

void pocket_input_pump(uint32_t buttons){
    int64_t now=0;
    uint32_t before=held_mask;
    held_mask=buttons;                    // held() works with no listener at all
    if(!action_table.open || !(buttons|before)) return;
    if(!now) now=esp_timer_get_time();
    for(int i=0;i<ACTION_COUNT;i++) {
        uint32_t bit=ACTIONS[i].bit;
        bool down=(buttons&bit)!=0, was=(before&bit)!=0;
        action_event_t event={.action=i,.now=now};
        if(down && !was) {
            event.phase="press";
            repeat_at[i]=now+REPEAT_DELAY_US;
        } else if(!down && was) {
            event.phase="release";
            repeat_at[i]=0;
        } else if(down && now>=repeat_at[i]) {
            event.phase="repeat";
            repeat_at[i]=now+REPEAT_PERIOD_US;
        } else continue;
        pocket_api_sub_deliver(&action_table,action_payload,&event);
    }
}

void pocket_input_reset(void){
    pocket_api_sub_close_all(&action_table);action_table.ctx=NULL;
    held_mask=0;memset(repeat_at,0,sizeof(repeat_at));
}
// Preserve the existing capability declaration during extraction. tick_run()
// now also forwards arrows and a final Back; reconcile this conservative
// declaration with scoped delivery when the input routing contract is revised.
static const pocket_limit_t input_limits[] = {
    {.name="actions",       .kind=POCKET_LIMIT_TEXT, .text="accept"},
    {.name="maxWatches",    .kind=POCKET_LIMIT_INT,  .number=UI_ACTION_SUBS},
    {.name="repeatDelayMs", .kind=POCKET_LIMIT_INT,  .number=REPEAT_DELAY_US/1000},
    {.name="keyEvents",     .kind=POCKET_LIMIT_FLAG, .number=0},
    {0},
};

static const pocket_capability_t input_capability = {
    .name="input.action", .supported=true, .available=true, .limits=input_limits,
};

static esp_err_t build_input(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    JS_DefinePropertyValueStr(ctx,ns,"onAction",
        JS_NewCFunction(ctx,js_on_action,"onAction",1),JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx,ns,"onKey",
        JS_NewCFunction(ctx,js_on_key,"onKey",1),JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx,ns,"held",
        JS_NewCFunction(ctx,js_held,"held",1),JS_PROP_ENUMERABLE);
    // input.text belongs to pocket_text.c, which contributes to this namespace
    // after this does. See the note where the stub used to be.
    return ESP_OK;
}

esp_err_t pocket_input_install(JSContext *ctx,void *user_data){
    (void)user_data;
    pocket_input_reset();
    for(unsigned i=0;i<UI_ACTION_SUBS;i++)action_slots[i].callback=JS_UNDEFINED;
    action_table.ctx=ctx;
    esp_err_t result=pocket_api_register(&input_capability);
    return result==ESP_OK?pocket_api_lazy(ctx,"input",build_input,NULL):result;
}
