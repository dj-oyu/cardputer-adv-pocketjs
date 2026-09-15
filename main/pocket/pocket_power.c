#include "pocket_power.h"
#include "pocket_api.h"
#include "board.h"
#include "system/sys_device.h"
#define POWER_WATCHES 2

// ------------------------------------------------------------------- power

// pocket_api.c owns the callback, the handle and the close(); what is left
// beside each subscription is whether it has been told anything yet.
static pocket_sub_slot_t power_slots[POWER_WATCHES];
static bool power_fresh[POWER_WATCHES];   // no delivery yet, so the first poll
                                          // reports whatever it finds
static void power_changed(pocket_sub_table_t *);
static pocket_sub_table_t power_table = {
    .slots=power_slots, .count=POWER_WATCHES,
    .tag="pocket.av", .what="power",
    // Same rule pocket_imu.c uses: a listener that throws loses its
    // subscription rather than the log and a call every second.
    .close_on_throw=true, .changed=power_changed,
};

static sys_sub power_subscription;
static void power_release(void){
    if(power_subscription.value)sys_unsubscribe(sys_device_state(),power_subscription);
    power_subscription=(sys_sub){0};
}

static void power_changed(pocket_sub_table_t *table){if(!table->open)power_release();}

// Section 8 refuses a state of charge guessed from one voltage of an
// uncharacterised cell, and the TP4057 on this board takes its charge status no
// further than the LED, so percent and charging are null here and say so in the
// power capability's limits. They are present rather than omitted because the
// section types them as `number|null` and `boolean|null`: a program checks the
// value, not the property.
static JSValue power_state(JSContext *ctx) {
    board_battery_t battery;
    bool have=board_battery_read(&battery);
    JSValue state=JS_NewObject(ctx);
    if(JS_IsException(state)) return state;
    JS_SetPropertyStr(ctx,state,"millivolts",
                      have?JS_NewInt32(ctx,battery.millivolts):JS_NULL);
    JS_SetPropertyStr(ctx,state,"percent",JS_NULL);
    JS_SetPropertyStr(ctx,state,"charging",JS_NULL);
    JS_SetPropertyStr(ctx,state,"timeMs",
                      have?JS_NewFloat64(ctx,battery.time_us/1000.0):JS_NULL);
    return state;
}

static JSValue js_status(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    return power_state(ctx);
}

static JSValue js_on_change(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv) {
    (void)this_val;
    if(argc<1 || !JS_IsFunction(ctx,argv[0]))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"power.onChange",
                                "onChange(listener) needs a function",false,NULL);
    bool reserved=false;
    if(!power_subscription.value){
        if(sys_subscribe(sys_device_state(),SYS_POWER,&power_subscription)!=SYS_OK)
            return pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,"power.onChange",
                                    "system subscriptions full",false,NULL);
        reserved=true;
    }
    int slot=0;
    JSValue subscription=pocket_api_sub_open(ctx,&power_table,argv[0],
                                             "power.onChange",
                                             "too many subscriptions",&slot);
    if(JS_IsException(subscription)){if(reserved)power_release();return subscription;}
    power_fresh[slot]=true;
    return subscription;
}

static JSValue js_keep_awake(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    // Section 2 keeps the method for a feature this build does not implement
    // and makes the call fail with UNSUPPORTED. There is nothing for it to hold
    // off: this firmware has no idle sleep and no backlight timeout, so a
    // keepAwake that returned a close() would be holding back nothing at all.
    // The power capability's limits say keepAwake=false for the same reason.
    return pocket_api_throw(ctx,POCKET_ERR_UNSUPPORTED,"power.keepAwake",
                            "this build never sleeps, so nothing can be held awake",
                            false,POCKET_OUTCOME_NOT_APPLIED);
}

static bool power_payload(JSContext *ctx, int slot, void *user, JSValue *payload) {
    const bool *changed=user;
    if(!*changed && !power_fresh[slot]) return false;
    power_fresh[slot]=false;
    sys_power_state sample;sys_power_read(sys_device_state(),&sample);
    *payload=JS_NewObject(ctx);
    if(JS_IsException(*payload))return true;
    JS_SetPropertyStr(ctx,*payload,"millivolts",sample.valid?JS_NewInt32(ctx,sample.millivolts):JS_NULL);
    JS_SetPropertyStr(ctx,*payload,"percent",JS_NULL);
    JS_SetPropertyStr(ctx,*payload,"charging",JS_NULL);
    JS_SetPropertyStr(ctx,*payload,"timeMs",sample.valid?JS_NewFloat64(ctx,sample.sampled_at/1000.0):JS_NULL);
    return true;
}

void pocket_power_pump(void) {
    if(!power_table.open){power_release();return;}
    sys_device_step();
    uint32_t dirty=0;sys_poll(sys_device_state(),power_subscription,&dirty);
    bool changed=(dirty&SYS_POWER)!=0;
    if(!changed){
        bool fresh=false;
        for(int i=0;i<POWER_WATCHES;i++)fresh|=power_slots[i].handle&&power_fresh[i];
        if(!fresh)return;
    }
    pocket_api_sub_deliver(&power_table,power_payload,&changed);
    if(!power_table.open)power_release();
}

static const pocket_limit_t power_limits[] = {
    {.name="millivolts",.kind=POCKET_LIMIT_FLAG,.number=1},
    {.name="percent",   .kind=POCKET_LIMIT_FLAG,.number=0},  // no cell curve
    {.name="charging",  .kind=POCKET_LIMIT_FLAG,.number=0},  // CHRG reaches the LED only
    {.name="keepAwake", .kind=POCKET_LIMIT_FLAG,.number=0},  // nothing sleeps yet
    {.name="maxWatches",.kind=POCKET_LIMIT_INT, .number=POWER_WATCHES},
    {.name="pollMs",    .kind=POCKET_LIMIT_INT, .number=SYS_POWER_PERIOD_US/1000},
    {0},
};

static void power_probe(const pocket_capability_t *cap, bool *available,
                        const char **reason) {
    (void)cap;
    board_battery_t battery;
    *available=board_battery_read(&battery);
    *reason=*available?NULL:POCKET_REASON_NO_DEVICE;
}

static const pocket_capability_t power_capability = {
    .name="power", .supported=true, .available=false,
    .reason=POCKET_REASON_NO_DEVICE, .limits=power_limits, .probe=power_probe,
};

static esp_err_t build_power(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    // A realm going away takes its callbacks with it, so the table starts empty
    // every time it is built.
    for(int i=0;i<POWER_WATCHES;i++) {
        power_slots[i].callback=JS_UNDEFINED;
        power_slots[i].handle=0;
    }
    power_table.open=0;
    power_table.ctx=ctx;
    power_subscription=(sys_sub){0};
    JS_DefinePropertyValueStr(ctx,ns,"status",
        JS_NewCFunction(ctx,js_status,"status",0),JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx,ns,"onChange",
        JS_NewCFunction(ctx,js_on_change,"onChange",1),JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx,ns,"keepAwake",
        JS_NewCFunction(ctx,js_keep_awake,"keepAwake",1),JS_PROP_ENUMERABLE);
    return ESP_OK;
}

void pocket_power_reset(void){
    pocket_api_sub_close_all(&power_table);
    power_table.ctx=NULL;power_release();
}
esp_err_t pocket_power_install(JSContext *ctx){
    pocket_api_register(&power_capability);
    return pocket_api_lazy(ctx,"power",build_power,NULL);
}
