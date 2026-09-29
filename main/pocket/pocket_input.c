#include "pocket_input.h"
#include "pocket_api.h"
#include "pocket_kasane.h"
#include "pocket_text.h"
#include "keymap.h"
#include "keystate.h"
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

// Section 6 gives onKey a key/code/modifiers shape: a stream of events with the
// typed character in it. That stream is still not delivered -- main.c's keymap
// hands the translated keystrokes to the shell and a running app a pad mask --
// and input.keys below answers the other question a game asks (what is down,
// what went down since the last frame) from keystate instead. Section 2 says an
// unimplemented feature keeps its name and fails with UNSUPPORTED, which is a
// better answer than a subscription that silently never fires.
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

// ------------------------------------------------------ pocket.input.keys
//
// The physical keys, read from keystate (main/hal/keystate.h) once per guest
// turn, here in the pump, and nowhere else: every call inside one frame() sees
// the same state, and pressed()/released() mean "since the previous turn's
// reading", counted, so a tap shorter than a frame is still one press and one
// release.
//
// A key that was already down when this surface last lost sight of the
// keyboard is hidden until it comes up. "Lost sight" is a re-base: the start
// of the session (the Enter that launched the app is usually still down),
// the wake from a suspension (the Enter that chose the sleeping app, and
// whatever the home screen was sent while it slept), and every turn in which
// the host had the keyboard instead of the app -- an open text field, a
// SYSTEM notice blocking input (the same test app_tick() zeroes the pad mask
// with), and a picker screen, which main.c reports because the guest is not
// ticked at all while it is up. Without that, the key that answered the
// picker would arrive in the app as a press, and one held across a sleep as
// a key that never came up.
static uint8_t  key_seen_press[KEYSTATE_KEYS], key_seen_release[KEYSTATE_KEYS];
static uint32_t key_hidden[KEYSTATE_WORDS];
static uint32_t key_down[KEYSTATE_WORDS], key_pressed[KEYSTATE_WORDS],
                key_released[KEYSTATE_WORDS];
static bool     key_rebase=true;

static void key_set(uint32_t *words,int i){words[i>>5]|=1u<<(i&31);}

static void keys_pump(void){
    keystate_snapshot_t now;
    keystate_snapshot(&now);
    bool rebase=key_rebase || pocket_text_active() ||
                pocket_kasane_input_scope(false)==KSN_INPUT_BLOCKED;
    key_rebase=false;
    memset(key_down,0,sizeof(key_down));
    memset(key_pressed,0,sizeof(key_pressed));
    memset(key_released,0,sizeof(key_released));
    if(rebase) memcpy(key_hidden,now.held,sizeof(key_hidden));
    else for(int i=0;i<KEYSTATE_KEYS;i++) {
        uint8_t presses=(uint8_t)(now.presses[i]-key_seen_press[i]);
        uint8_t releases=(uint8_t)(now.releases[i]-key_seen_release[i]);
        bool was_hidden=keystate_bit(key_hidden,i);
        if(releases) key_hidden[i>>5]&=~(1u<<(i&31));
        if(presses) key_set(key_pressed,i);
        // A hidden key's first release ends a press the app never saw.
        if(releases>(was_hidden?1:0)) key_set(key_released,i);
        if(keystate_bit(now.held,i) && !keystate_bit(key_hidden,i)) key_set(key_down,i);
    }
    memcpy(key_seen_press,now.presses,sizeof(key_seen_press));
    memcpy(key_seen_release,now.releases,sizeof(key_seen_release));
}

void pocket_input_keys_withhold(void){ key_rebase=true; }

static void keys_forget(void){
    key_rebase=true;
    memset(key_down,0,sizeof(key_down));
    memset(key_pressed,0,sizeof(key_pressed));
    memset(key_released,0,sizeof(key_released));
}

// held/pressed/released share everything but the mask they read, which is the
// function's magic.
static JSValue js_key_query(JSContext *ctx, JSValueConst self,
                            int argc, JSValueConst *argv, int magic) {
    (void)self;
    static const char *const OPS[]={"input.keys.held","input.keys.pressed",
                                    "input.keys.released"};
    const uint32_t *const MASKS[]={key_down,key_pressed,key_released};
    if(argc<1 || !JS_IsString(argv[0]))
        return bad(ctx,OPS[magic],"takes a key name");
    const char *name=JS_ToCString(ctx,argv[0]);
    if(!name) return JS_EXCEPTION;
    int index=keymap_key_index(name);
    JS_FreeCString(ctx,name);
    if(index<0) return bad(ctx,OPS[magic],"no such key");
    return JS_NewBool(ctx,keystate_bit(MASKS[magic],index));
}

static JSValue js_keys_down(JSContext *ctx, JSValueConst self,
                            int argc, JSValueConst *argv) {
    (void)self; (void)argc; (void)argv;
    JSValue list=JS_NewArray(ctx);
    if(JS_IsException(list)) return list;
    uint32_t at=0;
    for(int i=0;i<KEYSTATE_KEYS;i++) {
        char name[8];
        if(!keystate_bit(key_down,i) || !keymap_key_name(i,name)) continue;
        JSValue value=JS_NewString(ctx,name);
        if(JS_IsException(value) ||
           JS_DefinePropertyValueUint32(ctx,list,at++,value,JS_PROP_C_W_E)<0) {
            JS_FreeValue(ctx,list);
            return JS_EXCEPTION;
        }
    }
    return list;
}

// input.keys is built on its first read, like the namespaces themselves: an
// app that only uses onAction pays for one getter, not for an object and four
// functions. Same replace-the-accessor move as pocket_api.c's lazy namespace,
// and the same promise -- nothing half-built is installed.
static JSValue js_keys_build(JSContext *ctx, JSValueConst self,
                             int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    JSValue keys=JS_NewObject(ctx);
    if(JS_IsException(keys)) return keys;
    static const char *const QUERIES[]={"held","pressed","released"};
    bool ok=true;
    for(int i=0;i<3 && ok;i++)
        ok=JS_DefinePropertyValueStr(ctx,keys,QUERIES[i],
               JS_NewCFunctionMagic(ctx,js_key_query,QUERIES[i],1,
                                    JS_CFUNC_generic_magic,i),
               JS_PROP_ENUMERABLE)>=0;
    if(ok) ok=JS_DefinePropertyValueStr(ctx,keys,"down",
                  JS_NewCFunction(ctx,js_keys_down,"down",0),JS_PROP_ENUMERABLE)>=0;
    if(!ok) {
        JS_FreeValue(ctx,keys);
        return pocket_api_throw(ctx,POCKET_ERR_OUT_OF_MEMORY,"input.keys",
                                "no memory to build input.keys",true,NULL);
    }
    JS_DefinePropertyValueStr(ctx,self,"keys",JS_DupValue(ctx,keys),
                              JS_PROP_ENUMERABLE);
    return keys;
}

void pocket_input_pump(uint32_t buttons){
    keys_pump();
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

// Resident suspension: the Back that put the app to sleep is still "held" from
// the leave turn. Forgotten here, so the first turn after the wake does not
// report its release to an app that never saw the key come up.
void pocket_input_suspend(void){
    held_mask=0;memset(repeat_at,0,sizeof(repeat_at));
    keys_forget();
}

void pocket_input_reset(void){
    pocket_api_sub_close_all(&action_table);action_table.ctx=NULL;
    held_mask=0;memset(repeat_at,0,sizeof(repeat_at));
    keys_forget();
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
// No limits: how many keys the matrix reports down at once has not been
// measured on this board (docs/platform/keystate.md), and nothing here caps it.
static const pocket_capability_t keys_capability = {
    .name="input.keys", .supported=true, .available=true,
};

static esp_err_t build_input(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    JS_DefinePropertyValueStr(ctx,ns,"onAction",
        JS_NewCFunction(ctx,js_on_action,"onAction",1),JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx,ns,"onKey",
        JS_NewCFunction(ctx,js_on_key,"onKey",1),JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx,ns,"held",
        JS_NewCFunction(ctx,js_held,"held",1),JS_PROP_ENUMERABLE);
    // Configurable only until the first read replaces it (js_keys_build).
    JSAtom atom=JS_NewAtom(ctx,"keys");
    int defined=JS_DefinePropertyGetSet(ctx,ns,atom,
        JS_NewCFunction(ctx,js_keys_build,"keys",0),JS_UNDEFINED,
        JS_PROP_ENUMERABLE|JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx,atom);
    if(defined<0) return ESP_ERR_NO_MEM;
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
    if(result==ESP_OK) result=pocket_api_register(&keys_capability);
    return result==ESP_OK?pocket_api_lazy(ctx,"input",build_input,NULL):result;
}
