#include "pocket_imu.h"
#include "pocket_api.h"
#include "motion.h"
#include "esp_timer.h"
#include <math.h>

// Four is what fits the screen: a program that wants five views of the same
// sensor wants one watch and its own fan-out.
#define POCKET_IMU_WATCHES 4

// The pacing that sits beside each subscription. pocket_api.c owns the
// callback, the handle and the close(); what is left here is what a watch
// means -- how often it wants a sample, and how many it has been spared.
typedef struct {
    int64_t  period_us;
    int64_t  next_us;
    uint32_t last_sequence;   // 0 until the first delivery
    uint32_t dropped;         // deliveries omitted since this watch opened
} watch_t;

static pocket_sub_slot_t watch_slots[POCKET_IMU_WATCHES];
static watch_t           watches[POCKET_IMU_WATCHES];

// The gyroscope draws several times the accelerometer's current, so it runs
// only while something is watching. latest() therefore reports gyro null to a
// program that never opened a watch, which is the same "not available now"
// answer section 8 gives for a sensor the firmware does not read.
static void follow_gyro_demand(pocket_sub_table_t *table) {
    motion_request_gyro(table->open>0);
}

static pocket_sub_table_t watch_table = {
    .slots=watch_slots, .count=POCKET_IMU_WATCHES,
    .tag="pocket.imu", .what="watch",
    // A listener that throws every frame would otherwise fill the log and keep
    // costing a call; the program keeps its other watches.
    .close_on_throw=true, .changed=follow_gyro_demand,
};

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
    int slot=0;
    JSValue subscription=pocket_api_sub_open(ctx,&watch_table,argv[1],
                                             "sensors.imu.watch",
                                             "too many watches",&slot);
    if(JS_IsException(subscription)) return subscription;
    watches[slot]=(watch_t){
        .period_us=(int64_t)(1000000.0/rate),
        .next_us=esp_timer_get_time(),
    };
    return subscription;
}

// One frame's sample, and the time the frame asked for it.
typedef struct {
    const motion_sample_t *sample;
    int64_t                now;
} watch_round_t;

static bool watch_payload(JSContext *ctx, int slot, void *user, JSValue *payload) {
    const watch_round_t *round=user;
    watch_t             *w=&watches[slot];
    if(round->now<w->next_us) return false;
    // The rate is a ceiling on deliveries, not a promise of one: with nothing
    // new from the sensor there is nothing to deliver, and re-sending the last
    // sample would make timeMs a lie about freshness.
    if(w->last_sequence==round->sample->sequence) return false;
    // Samples the sensor produced between two deliveries were skipped on
    // purpose -- this is the "delivered the newest, queued nothing" part of
    // section 8, and dropped is how the program learns it happened.
    if(w->last_sequence) w->dropped+=round->sample->sequence-w->last_sequence-1;
    w->last_sequence=round->sample->sequence;
    // Drift-free pacing, but never a burst: after a long stall the next
    // delivery is one period out rather than several at once.
    w->next_us+=w->period_us;
    if(w->next_us<round->now) w->next_us=round->now+w->period_us;
    *payload=sample_object(ctx,round->sample,w->dropped);
    return true;
}

void pocket_imu_pump(void) {
    if(!watch_table.open) return;
    motion_sample_t sample;
    if(!motion_latest(&sample)) return;
    watch_round_t round={.sample=&sample,.now=esp_timer_get_time()};
    pocket_api_sub_deliver(&watch_table,watch_payload,&round);
}

// Whether pocket.sensors was ever read.
static bool built;

void pocket_imu_reset(void) {
    // A run that never read the namespace opened no watch and asked the
    // gyroscope for nothing, so there is nothing here to put back.
    if(!built) return;
    built=false;
    pocket_api_sub_close_all(&watch_table);
    watch_table.ctx=NULL;
    // A program that ended without closing its watches must not leave the
    // gyroscope drawing current until the next one starts. Closing them has
    // already done that through `changed`; this is for the run that opened
    // none, whose install still asked the gyroscope for nothing.
    motion_request_gyro(false);
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

static esp_err_t build_sensors(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    for(int i=0;i<POCKET_IMU_WATCHES;i++) {
        // A realm going away takes its callbacks with it. Nothing here survives
        // a session, so the table starts empty every time it is built.
        watch_slots[i].callback=JS_UNDEFINED;
        watch_slots[i].handle=0;
    }
    watch_table.open=0;
    watch_table.ctx=ctx;
    follow_gyro_demand(&watch_table);

    JSValue imu=JS_NewObject(ctx);
    if(JS_IsException(imu)) return ESP_ERR_NO_MEM;
    JS_SetPropertyStr(ctx,imu,"latest",JS_NewCFunction(ctx,js_latest,"latest",0));
    JS_SetPropertyStr(ctx,imu,"watch",JS_NewCFunction(ctx,js_watch,"watch",2));
    JS_SetPropertyStr(ctx,ns,"imu",imu);
    built=true;
    return ESP_OK;
}

esp_err_t pocket_imu_install(JSContext *ctx, void *user_data) {
    (void)user_data;
    // maxRateHz is what motion.c actually polls at, so the published limit and
    // the limit js_watch enforces are the same number by construction. Eager,
    // with the capability, so a feature test reads a live limit without the
    // namespace existing.
    for(unsigned i=0;i<sizeof(imu_limits)/sizeof(imu_limits[0]);i++)
        imu_limits_live[i]=imu_limits[i];
    imu_limits_live[0].number=(int32_t)motion_rate_hz();
    pocket_api_register(&imu_capability);
    return pocket_api_lazy(ctx,"sensors",build_sensors,NULL);
}
