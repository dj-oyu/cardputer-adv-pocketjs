#include "pocket_imu.h"
#include "pocket_api.h"
#include "motion.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <math.h>

// Four is what fits the screen: a program that wants five views of the same
// sensor wants one watch and its own fan-out.
#define POCKET_IMU_WATCHES 4

typedef struct {
    JSValue  callback;
    int64_t  period_us;
    int64_t  next_us;
    uint32_t last_sequence;   // 0 until the first delivery
    uint32_t dropped;         // deliveries omitted since this watch opened
    uint32_t handle;          // 0 marks a free slot
} watch_t;

static JSContext *watch_ctx;
static watch_t    watches[POCKET_IMU_WATCHES];
static unsigned   open_watches;
static uint32_t   next_handle;

static const char *TAG = "pocket.imu";

// The gyroscope draws several times the accelerometer's current, so it runs
// only while something is watching. latest() therefore reports gyro null to a
// program that never opened a watch, which is the same "not available now"
// answer section 8 gives for a sensor the firmware does not read.
static void follow_gyro_demand(void) {
    motion_request_gyro(open_watches>0);
}

static JSValue sample_object(JSContext *ctx, const motion_sample_t *s,
                             uint32_t dropped) {
    JSValue object=JS_NewObject(ctx);
    if(JS_IsException(object)) return object;
    JS_SetPropertyStr(ctx,object,"timeMs",JS_NewFloat64(ctx,s->time_us/1000.0));
    JS_SetPropertyStr(ctx,object,"sequence",JS_NewUint32(ctx,s->sequence));
    JS_SetPropertyStr(ctx,object,"dropped",JS_NewUint32(ctx,dropped));
    JSValue accel=JS_NewObject(ctx);
    JS_SetPropertyStr(ctx,accel,"x",JS_NewFloat64(ctx,s->accel_x));
    JS_SetPropertyStr(ctx,accel,"y",JS_NewFloat64(ctx,s->accel_y));
    JS_SetPropertyStr(ctx,accel,"z",JS_NewFloat64(ctx,s->accel_z));
    JS_SetPropertyStr(ctx,object,"accel",accel);
    if(s->gyro_valid) {
        JSValue gyro=JS_NewObject(ctx);
        JS_SetPropertyStr(ctx,gyro,"x",JS_NewFloat64(ctx,s->gyro_x));
        JS_SetPropertyStr(ctx,gyro,"y",JS_NewFloat64(ctx,s->gyro_y));
        JS_SetPropertyStr(ctx,gyro,"z",JS_NewFloat64(ctx,s->gyro_z));
        JS_SetPropertyStr(ctx,object,"gyro",gyro);
    } else {
        JS_SetPropertyStr(ctx,object,"gyro",JS_NULL);
    }
    // The spec calls roll and pitch a resting inclination and refuses to
    // promise them as attitude, so they travel as tilt and stay together.
    JSValue tilt=JS_NewObject(ctx);
    JS_SetPropertyStr(ctx,tilt,"roll",JS_NewFloat64(ctx,s->roll));
    JS_SetPropertyStr(ctx,tilt,"pitch",JS_NewFloat64(ctx,s->pitch));
    JS_SetPropertyStr(ctx,object,"tilt",tilt);
    return object;
}

static JSValue js_latest(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    motion_sample_t sample;
    // Null, not an error: a program polling before the first 20 ms tick has
    // done nothing wrong, and section 8 types the return as ImuSample|null.
    if(!motion_latest(&sample)) return JS_NULL;
    return sample_object(ctx,&sample,sample.dropped);
}

static void close_slot(JSContext *ctx, int slot) {
    if(!watches[slot].handle) return;
    watches[slot].handle=0;
    JS_FreeValue(ctx,watches[slot].callback);
    watches[slot].callback=JS_UNDEFINED;
    open_watches--;
    follow_gyro_demand();
}

static JSValue js_watch_close(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int magic,
                              JSValueConst *func_data) {
    (void)this_val; (void)argc; (void)argv;
    uint32_t handle=0;
    if(JS_ToUint32(ctx,&handle,func_data[0])) return JS_EXCEPTION;
    // Bound to the slot and to the handle that slot held, so closing twice, or
    // closing after the slot has been reused, does nothing.
    if(magic>=0 && magic<POCKET_IMU_WATCHES && handle && watches[magic].handle==handle)
        close_slot(ctx,magic);
    return JS_UNDEFINED;
}

static JSValue js_watch(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv) {
    (void)this_val;
    if(argc<2 || !JS_IsFunction(ctx,argv[1]))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"sensors.imu.watch",
                                "watch(options, listener) needs a function",false,NULL);
    if(!motion_present())
        return pocket_api_throw(ctx,POCKET_ERR_NOT_AVAILABLE,"sensors.imu.watch",
                                "no IMU on this unit",false,NULL);
    double rate=0;
    JSValue field=JS_GetPropertyStr(ctx,argv[0],"rateHz");
    if(JS_ToFloat64(ctx,&rate,field)) { JS_FreeValue(ctx,field); return JS_EXCEPTION; }
    JS_FreeValue(ctx,field);
    if(!(rate>0))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"sensors.imu.watch",
                                "rateHz must be positive",false,NULL);
    // A rate the sensor is not read at cannot be served, and section 8 asks for
    // that to be refused at open rather than silently delivered slower.
    if(rate>motion_rate_hz())
        return pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,"sensors.imu.watch",
                                "rateHz above the sensor's rate",false,NULL);
    int slot=-1;
    for(int i=0;i<POCKET_IMU_WATCHES;i++)
        if(!watches[i].handle) { slot=i; break; }
    if(slot<0)
        return pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,"sensors.imu.watch",
                                "too many watches",false,NULL);
    JSValue subscription=JS_NewObject(ctx);
    if(JS_IsException(subscription)) return subscription;
    if(++next_handle==0) next_handle=1;
    watches[slot]=(watch_t){
        .callback=JS_DupValue(ctx,argv[1]),
        .period_us=(int64_t)(1000000.0/rate),
        .next_us=esp_timer_get_time(),
        .handle=next_handle,
    };
    watch_ctx=ctx;
    open_watches++;
    follow_gyro_demand();
    JSValue handle=JS_NewUint32(ctx,watches[slot].handle);
    JSValue close=JS_NewCFunctionData(ctx,js_watch_close,0,slot,1,&handle);
    JS_FreeValue(ctx,handle);
    JS_SetPropertyStr(ctx,subscription,"close",close);
    return subscription;
}

void pocket_imu_pump(void) {
    if(!open_watches || !watch_ctx) return;
    int64_t now=esp_timer_get_time();
    motion_sample_t sample;
    if(!motion_latest(&sample)) return;
    JSContext *ctx=watch_ctx;
    for(int i=0;i<POCKET_IMU_WATCHES;i++) {
        watch_t *w=&watches[i];
        if(!w->handle || now<w->next_us) continue;
        // The rate is a ceiling on deliveries, not a promise of one: with
        // nothing new from the sensor there is nothing to deliver, and
        // re-sending the last sample would make timeMs a lie about freshness.
        if(w->last_sequence==sample.sequence) continue;
        // Samples the sensor produced between two deliveries were skipped on
        // purpose -- this is the "delivered the newest, queued nothing" part of
        // section 8, and dropped is how the program learns it happened.
        if(w->last_sequence) w->dropped+=sample.sequence-w->last_sequence-1;
        w->last_sequence=sample.sequence;
        // Drift-free pacing, but never a burst: after a long stall the next
        // delivery is one period out rather than several at once.
        w->next_us+=w->period_us;
        if(w->next_us<now) w->next_us=now+w->period_us;
        uint32_t handle=w->handle;
        JSValue fn=JS_DupValue(ctx,w->callback);
        JSValue payload=sample_object(ctx,&sample,w->dropped);
        JSValue result=JS_Call(ctx,fn,JS_UNDEFINED,1,(JSValueConst *)&payload);
        if(JS_IsException(result)) {
            JSValue error=JS_GetException(ctx);
            const char *text=JS_ToCString(ctx,error);
            ESP_LOGW(TAG,"watch listener failed: %s",text?text:"?");
            if(text) JS_FreeCString(ctx,text);
            JS_FreeValue(ctx,error);
            // A listener that throws every frame would otherwise fill the log
            // and keep costing a call; the program keeps its other watches.
            if(w->handle==handle) close_slot(ctx,i);
        }
        JS_FreeValue(ctx,result);
        JS_FreeValue(ctx,payload);
        JS_FreeValue(ctx,fn);
    }
}

void pocket_imu_reset(void) {
    if(watch_ctx)
        for(int i=0;i<POCKET_IMU_WATCHES;i++) close_slot(watch_ctx,i);
    watch_ctx=NULL;
    open_watches=0;
    // A program that ended without closing its watches must not leave the
    // gyroscope drawing current until the next one starts.
    follow_gyro_demand();
}

// ---------------------------------------------------------------- capability

static const pocket_limit_t imu_limits[] = {
    {.name="maxRateHz", .kind=POCKET_LIMIT_INT,  .number=0},   // filled at init
    {.name="maxWatches",.kind=POCKET_LIMIT_INT,  .number=POCKET_IMU_WATCHES},
    {.name="gyro",      .kind=POCKET_LIMIT_FLAG, .number=1},
    {.name="magnetometer",.kind=POCKET_LIMIT_FLAG,.number=0},  // BMI270 is 6-axis
    {0},
};
static pocket_limit_t imu_limits_live[sizeof(imu_limits)/sizeof(imu_limits[0])];

static void imu_probe(const pocket_capability_t *cap, bool *available,
                      const char **reason) {
    (void)cap;
    *available=motion_present();
    *reason=*available?NULL:POCKET_REASON_NO_DEVICE;
}

static const pocket_capability_t imu_capability = {
    .name="sensors.imu", .supported=true, .available=false,
    .reason=POCKET_REASON_NO_DEVICE, .limits=imu_limits_live, .probe=imu_probe,
};

esp_err_t pocket_imu_install(JSContext *ctx, void *user_data) {
    (void)user_data;
    // maxRateHz is what motion.c actually polls at, so the published limit and
    // the limit js_watch enforces are the same number by construction.
    for(unsigned i=0;i<sizeof(imu_limits)/sizeof(imu_limits[0]);i++)
        imu_limits_live[i]=imu_limits[i];
    imu_limits_live[0].number=(int32_t)motion_rate_hz();
    pocket_api_register(&imu_capability);

    for(int i=0;i<POCKET_IMU_WATCHES;i++) {
        // A realm going away takes its callbacks with it. Nothing here survives
        // a session, so the table starts empty on every install.
        watches[i].callback=JS_UNDEFINED;
        watches[i].handle=0;
    }
    open_watches=0;
    watch_ctx=ctx;
    follow_gyro_demand();

    JSValue root=pocket_api_root(ctx);
    if(JS_IsUndefined(root)) { JS_FreeValue(ctx,root); return ESP_ERR_INVALID_STATE; }
    JSValue sensors=JS_NewObject(ctx);
    JSValue imu=JS_NewObject(ctx);
    JS_SetPropertyStr(ctx,imu,"latest",JS_NewCFunction(ctx,js_latest,"latest",0));
    JS_SetPropertyStr(ctx,imu,"watch",JS_NewCFunction(ctx,js_watch,"watch",2));
    JS_SetPropertyStr(ctx,sensors,"imu",imu);
    JS_DefinePropertyValueStr(ctx,root,"sensors",sensors,JS_PROP_ENUMERABLE);
    JS_FreeValue(ctx,root);
    return ESP_OK;
}
