#include "pocket_av.h"
#include "pocket_api.h"
#include "sound.h"
#include "board.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "pocket.av";

// Section 14 caps a general Promise at 30000ms; storage.kv enforces the same
// number, and one ceiling for the whole host is easier to teach than one per
// API. A tone can wait behind another tone for up to its full length, so the
// default is the tone's own duration plus a second of queue.
#define AV_MAX_TIMEOUT_MS 30000
#define AV_QUEUE_SLACK_MS 1000

// Two is what the screen can use: a battery indicator and whatever else the
// program draws. A third view of one number is the program's own fan-out.
#define POWER_WATCHES 2
// The ADC is averaged and cached for 500ms inside board_battery_read, so one
// second is the finest poll that costs a conversion, and the pack does not move
// faster than that. 20mV is above the spread two consecutive averages show and
// below anything a program would draw differently.
#define POWER_POLL_US   1000000
#define POWER_STEP_MV   20

static JSContext *av_ctx;

// ------------------------------------------------------------------ promises
//
// Section 4 puts argument errors from a Promise-returning method into the
// rejection rather than a throw. Same two helpers pocket_storage.c has; see the
// note at the end of this file about where they should end up living.

static JSValue settled(JSContext *ctx, JSValue value, bool rejected) {
    JSValue funcs[2];
    JSValue promise=JS_NewPromiseCapability(ctx,funcs);
    if(JS_IsException(promise)) { JS_FreeValue(ctx,value); return promise; }
    JSValue done=JS_Call(ctx,funcs[rejected?1:0],JS_UNDEFINED,1,
                         (JSValueConst *)&value);
    JS_FreeValue(ctx,done);
    JS_FreeValue(ctx,funcs[0]);
    JS_FreeValue(ctx,funcs[1]);
    JS_FreeValue(ctx,value);
    return promise;
}

static JSValue reject(JSContext *ctx, const char *code, const char *operation,
                      const char *message, bool retryable, const char *outcome) {
    JSValue error=pocket_api_error(ctx,code,operation,message,retryable,outcome);
    if(JS_IsException(error)) return error;   // only on OOM building the error
    return settled(ctx,error,true);
}

// ------------------------------------------------------------------- options

typedef struct {
    int32_t timeout_ms;    // 0 for "not given"
    JSValue cancel;        // JS_UNDEFINED when none; the caller frees it
    bool    cancelled;     // the token was already cancelled at the call
} av_options_t;

// Unlike storage's copy this one keeps the token: a tone lasts up to five
// seconds, so section 4's cancel has to be polled for the whole of it rather
// than observed once at the call.
static JSValue take_options(JSContext *ctx, JSValueConst value,
                            const char *operation, av_options_t *out) {
    out->timeout_ms=0;
    out->cancel=JS_UNDEFINED;
    out->cancelled=false;
    if(JS_IsUndefined(value) || JS_IsNull(value)) return JS_UNDEFINED;
    if(!JS_IsObject(value))
        return reject(ctx,POCKET_ERR_INVALID_ARGUMENT,operation,
                      "options must be an object",false,
                      POCKET_OUTCOME_NOT_APPLIED);

    JSValue timeout=JS_GetPropertyStr(ctx,value,"timeoutMs");
    if(JS_IsException(timeout)) return JS_EXCEPTION;
    if(!JS_IsUndefined(timeout)) {
        double ms=0;
        bool bad=!JS_IsNumber(timeout) || JS_ToFloat64(ctx,&ms,timeout);
        JS_FreeValue(ctx,timeout);
        // Section 4 refuses to round an over-range request quietly.
        if(bad || !isfinite(ms) || ms!=(double)(int64_t)ms ||
           ms<1 || ms>AV_MAX_TIMEOUT_MS)
            return reject(ctx,POCKET_ERR_INVALID_ARGUMENT,operation,
                          "timeoutMs must be a whole number of 1 to 30000",
                          false,POCKET_OUTCOME_NOT_APPLIED);
        out->timeout_ms=(int32_t)ms;
    } else JS_FreeValue(ctx,timeout);

    JSValue cancel=JS_GetPropertyStr(ctx,value,"cancel");
    if(JS_IsException(cancel)) return JS_EXCEPTION;
    if(!JS_IsUndefined(cancel) && !JS_IsNull(cancel)) {
        if(!pocket_api_is_cancel_token(cancel)) {
            JS_FreeValue(ctx,cancel);
            return reject(ctx,POCKET_ERR_INVALID_ARGUMENT,operation,
                          "cancel must be a token from pocket.cancel.source()",
                          false,POCKET_OUTCOME_NOT_APPLIED);
        }
        out->cancelled=pocket_api_cancel_requested(cancel);
        out->cancel=cancel;      // kept, and freed when the tone settles
        return JS_UNDEFINED;
    }
    JS_FreeValue(ctx,cancel);
    return JS_UNDEFINED;
}

// --------------------------------------------------------------- audio.cue

static const char *const CUE_NAMES[] = { "move", "accept", "back" };

static JSValue js_cue(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv) {
    (void)this_val;
    const char *name=argc>0?JS_ToCString(ctx,argv[0]):NULL;
    if(!name)
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"audio.cue",
                                "cue(name) needs a string",false,NULL);
    int kind=-1;
    for(int i=0;i<(int)(sizeof(CUE_NAMES)/sizeof(CUE_NAMES[0]));i++)
        if(!strcmp(name,CUE_NAMES[i])) { kind=i; break; }
    JS_FreeCString(ctx,name);
    // A name outside the three is an argument error, not a false: false means
    // "the queue would not take it", and section 9 lets a program tell the two
    // apart by having only the second come back as a value.
    if(kind<0)
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"audio.cue",
                                "name must be move, accept or back",false,NULL);
    // sound_play returns false when muted or when the queue is full, which is
    // exactly what section 9 asks cue to report; it never means it was heard.
    return JS_NewBool(ctx,sound_play(kind));
}

// -------------------------------------------------------------- audio.tone
//
// The hand-back. sound.c calls `done` on the audio task at priority 7, and
// section 5 forbids that task from touching QuickJS at all, so the callback
// writes two atomics and stops. pocket_av_pump() reads them on the JS task and
// is the only code that settles the Promise.
//
// The request number, not a flag, is what the two sides agree on: sound_tone()
// may finish a 1ms tone before it has even returned its id, and a session may
// end with a tone still sounding. Matching numbers makes both harmless -- a
// completion whose number nobody is waiting for is simply never read. The
// counter is deliberately never reset, so a tone left behind by one session
// cannot be mistaken for the first tone of the next.

static atomic_uint tone_finished;             // request number, 0 for none
static atomic_bool tone_finished_completed;

static uint32_t tone_next_request = 1;

static struct {
    bool        active;
    JSValue     resolve, reject;
    JSValue     cancel;         // JS_UNDEFINED when the call passed no token
    uint32_t    request;
    int32_t     id;             // sound.c's id, for sound_tone_cancel
    int64_t     deadline_us;
    const char *stop_code;      // NULL until we asked the tone to stop early
} tone;

static void tone_done(void *ctx, bool completed) {
    atomic_store(&tone_finished_completed,completed);
    // Written last: the pump reads the number first and only then trusts the
    // flag beside it.
    atomic_store(&tone_finished,(uint32_t)(uintptr_t)ctx);
}

static void tone_release(JSContext *ctx) {
    JS_FreeValue(ctx,tone.resolve);
    JS_FreeValue(ctx,tone.reject);
    JS_FreeValue(ctx,tone.cancel);
    tone.resolve=tone.reject=tone.cancel=JS_UNDEFINED;
    tone.active=false;
    tone.stop_code=NULL;
}

static void tone_settle(JSContext *ctx, JSValue value, bool rejected) {
    // pocket_api_error() can only fail by running the guest heap out while
    // building the error. Settling with whatever QuickJS threw instead keeps
    // the rule that matters: a Promise handed to the app always settles.
    if(JS_IsException(value)) value=JS_GetException(ctx);
    JSValue done=JS_Call(ctx,rejected?tone.reject:tone.resolve,JS_UNDEFINED,1,
                         (JSValueConst *)&value);
    JS_FreeValue(ctx,done);
    JS_FreeValue(ctx,value);
    tone_release(ctx);
}

static JSValue js_tone(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv) {
    (void)this_val;
    static const char *const OP="audio.tone";
    if(argc<1 || !JS_IsObject(argv[0]))
        return reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                      "tone(spec, options) needs a spec object",false,
                      POCKET_OUTCOME_NOT_APPLIED);

    double frequency=0, duration=0, gain=0;
    static const char *const FIELDS[]={"frequencyHz","durationMs","gain"};
    double *const SLOTS[]={&frequency,&duration,&gain};
    for(int i=0;i<3;i++) {
        JSValue field=JS_GetPropertyStr(ctx,argv[0],FIELDS[i]);
        if(JS_IsException(field)) return JS_EXCEPTION;
        bool bad=!JS_IsNumber(field) || JS_ToFloat64(ctx,SLOTS[i],field);
        JS_FreeValue(ctx,field);
        if(bad || !isfinite(*SLOTS[i]))
            return reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                          "frequencyHz, durationMs and gain must be numbers",
                          false,POCKET_OUTCOME_NOT_APPLIED);
    }
    // The same range sound.c enforces, checked here so the refusal arrives as a
    // PocketError with the limit in it rather than as SOUND_ERR_INVALID. The
    // rounding below is the 1Hz and 1ms resolution of the synthesiser, not a
    // silent clamp: a value outside the range is refused, never rounded into it.
    if(frequency<SOUND_TONE_MIN_HZ || frequency>SOUND_TONE_MAX_HZ)
        return reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                      "frequencyHz must be 20 to 8000",false,
                      POCKET_OUTCOME_NOT_APPLIED);
    if(duration<1 || duration>SOUND_TONE_MAX_MS)
        return reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                      "durationMs must be 1 to 5000",false,
                      POCKET_OUTCOME_NOT_APPLIED);
    // Written as a positive test so a NaN gain cannot pass two false compares.
    if(!(gain>=0.0 && gain<=1.0))
        return reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                      "gain must be 0 to 1",false,POCKET_OUTCOME_NOT_APPLIED);

    av_options_t options;
    JSValue bad=take_options(ctx,argc>1?argv[1]:JS_UNDEFINED,OP,&options);
    if(!JS_IsUndefined(bad)) return bad;

    // Section 4 allows one operation of a kind per handle and answers the
    // second with BUSY. One tone at a time is also what sound.c can cancel:
    // it remembers a single id.
    if(tone.active) {
        JS_FreeValue(ctx,options.cancel);
        return reject(ctx,POCKET_ERR_BUSY,OP,"a tone is already playing",true,
                      POCKET_OUTCOME_NOT_APPLIED);
    }
    if(options.cancelled) {
        JS_FreeValue(ctx,options.cancel);
        return reject(ctx,POCKET_ERR_CANCELLED,OP,"cancelled before the tone",
                      false,POCKET_OUTCOME_NOT_APPLIED);
    }

    unsigned hz=(unsigned)(frequency+0.5), ms=(unsigned)(duration+0.5);
    uint32_t request=tone_next_request;
    int32_t id=sound_tone(hz,ms,(float)gain,tone_done,(void *)(uintptr_t)request);
    if(id<0) {
        JS_FreeValue(ctx,options.cancel);
        switch(id) {
            case SOUND_ERR_BUSY:
                return reject(ctx,POCKET_ERR_BUSY,OP,"the sound queue is full",
                              true,POCKET_OUTCOME_NOT_APPLIED);
            // sound.c calls this one UNSUPPORTED because it means "no codec on
            // this board", but section 2 reserves UNSUPPORTED for what the
            // firmware does not implement. A missing codec is NOT_AVAILABLE.
            case SOUND_ERR_UNSUPPORTED:
                return reject(ctx,POCKET_ERR_NOT_AVAILABLE,OP,
                              "no audio codec on this unit",false,
                              POCKET_OUTCOME_NOT_APPLIED);
            default:
                return reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                              "the tone was refused",false,
                              POCKET_OUTCOME_NOT_APPLIED);
        }
    }
    if(++tone_next_request==0) tone_next_request=1;

    JSValue funcs[2];
    JSValue promise=JS_NewPromiseCapability(ctx,funcs);
    if(JS_IsException(promise)) {
        // Nothing can settle a Promise that was not built, so the tone is asked
        // to stop; its completion goes unread, which is what an unmatched
        // request number is for.
        sound_tone_cancel(id);
        JS_FreeValue(ctx,options.cancel);
        return promise;
    }
    tone.active=true;
    tone.resolve=funcs[0];
    tone.reject=funcs[1];
    tone.cancel=options.cancel;
    tone.request=request;
    tone.id=id;
    // Section 4 measures timeoutMs from the call and includes the queue wait,
    // so the default has to cover a tone already sounding ahead of this one.
    tone.deadline_us=esp_timer_get_time()+
        1000LL*(options.timeout_ms?options.timeout_ms:(int32_t)ms+AV_QUEUE_SLACK_MS);
    av_ctx=ctx;
    return promise;
}

// Asks the tone to stop and remembers why. The Promise is not settled here:
// section 4 gives the host the wait for the native stop, and the audio task
// still owns the request until its callback lands.
static void tone_stop(const char *code) {
    if(tone.stop_code) return;
    tone.stop_code=code;
    sound_tone_cancel(tone.id);
}

static void tone_pump(JSContext *ctx) {
    uint32_t finished=atomic_load(&tone_finished);
    if(finished==tone.request) {
        bool completed=atomic_load(&tone_finished_completed);
        atomic_store(&tone_finished,0);
        if(tone.stop_code) {
            // The tone was told to stop. outcome is the honest part: a stop
            // that arrived after the last frame still made the sound.
            tone_settle(ctx,pocket_api_error(ctx,tone.stop_code,"audio.tone",
                !strcmp(tone.stop_code,POCKET_ERR_TIMEOUT)
                    ?"the tone outlived timeoutMs":"cancelled while playing",
                false,completed?POCKET_OUTCOME_APPLIED:POCKET_OUTCOME_UNKNOWN),true);
        } else if(completed) {
            tone_settle(ctx,JS_UNDEFINED,false);
        } else {
            // Nobody asked it to stop, so the I2S write failed under it.
            tone_settle(ctx,pocket_api_error(ctx,POCKET_ERR_IO_ERROR,"audio.tone",
                "the tone stopped early",true,POCKET_OUTCOME_UNKNOWN),true);
        }
        return;
    }
    if(tone.stop_code) return;    // already stopping; waiting for the callback
    if(!JS_IsUndefined(tone.cancel) && pocket_api_cancel_requested(tone.cancel))
        tone_stop(POCKET_ERR_CANCELLED);
    else if(esp_timer_get_time()>tone.deadline_us)
        tone_stop(POCKET_ERR_TIMEOUT);
}

// ------------------------------------------------------------------- power

typedef struct {
    JSValue  callback;
    uint32_t handle;      // 0 marks a free slot
    bool     fresh;       // no delivery yet, so the first poll reports whatever it finds
} power_watch_t;

static power_watch_t power_watches[POWER_WATCHES];
static unsigned      power_open;
static uint32_t      power_next_handle;
static int64_t       power_next_us;
static bool          power_primed;   // a sample has been taken since install
static bool          power_have;     // that sample was readable
static int           power_mv;

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

static void power_close_slot(JSContext *ctx, int slot) {
    if(!power_watches[slot].handle) return;
    power_watches[slot].handle=0;
    JS_FreeValue(ctx,power_watches[slot].callback);
    power_watches[slot].callback=JS_UNDEFINED;
    power_open--;
}

static JSValue js_power_close(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int magic,
                              JSValueConst *func_data) {
    (void)this_val; (void)argc; (void)argv;
    uint32_t handle=0;
    if(JS_ToUint32(ctx,&handle,func_data[0])) return JS_EXCEPTION;
    // Bound to the slot and to the handle it held, so closing twice, or closing
    // after the slot was reused, does nothing.
    if(magic>=0 && magic<POWER_WATCHES && handle &&
       power_watches[magic].handle==handle)
        power_close_slot(ctx,magic);
    return JS_UNDEFINED;
}

static JSValue js_on_change(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv) {
    (void)this_val;
    if(argc<1 || !JS_IsFunction(ctx,argv[0]))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"power.onChange",
                                "onChange(listener) needs a function",false,NULL);
    int slot=-1;
    for(int i=0;i<POWER_WATCHES;i++)
        if(!power_watches[i].handle) { slot=i; break; }
    if(slot<0)
        return pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,"power.onChange",
                                "too many subscriptions",false,NULL);
    JSValue subscription=JS_NewObject(ctx);
    if(JS_IsException(subscription)) return subscription;
    if(++power_next_handle==0) power_next_handle=1;
    power_watches[slot]=(power_watch_t){
        .callback=JS_DupValue(ctx,argv[0]),
        .handle=power_next_handle,
        .fresh=true,
    };
    av_ctx=ctx;
    power_open++;
    power_next_us=0;    // sample on the next frame rather than a second later
    JSValue handle=JS_NewUint32(ctx,power_watches[slot].handle);
    JSValue close=JS_NewCFunctionData(ctx,js_power_close,0,slot,1,&handle);
    JS_FreeValue(ctx,handle);
    JS_SetPropertyStr(ctx,subscription,"close",close);
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

static void power_pump(JSContext *ctx) {
    int64_t now=esp_timer_get_time();
    if(now<power_next_us) return;
    power_next_us=now+POWER_POLL_US;
    board_battery_t battery;
    bool have=board_battery_read(&battery);
    // A change is the reading appearing, disappearing, or moving further than
    // the ADC's own spread. Without the step every subscriber would be woken
    // once a second to be told the same voltage.
    bool changed=!power_primed || have!=power_have ||
                 (have && abs(battery.millivolts-power_mv)>=POWER_STEP_MV);
    power_primed=true;
    if(changed) { power_have=have; power_mv=have?battery.millivolts:0; }

    for(int i=0;i<POWER_WATCHES;i++) {
        power_watch_t *w=&power_watches[i];
        if(!w->handle || (!changed && !w->fresh)) continue;
        w->fresh=false;
        uint32_t handle=w->handle;
        JSValue fn=JS_DupValue(ctx,w->callback);
        JSValue payload=power_state(ctx);
        JSValue result=JS_Call(ctx,fn,JS_UNDEFINED,1,(JSValueConst *)&payload);
        if(JS_IsException(result)) {
            JSValue error=JS_GetException(ctx);
            const char *text=JS_ToCString(ctx,error);
            ESP_LOGW(TAG,"power listener failed: %s",text?text:"?");
            if(text) JS_FreeCString(ctx,text);
            JS_FreeValue(ctx,error);
            // Same rule pocket_imu.c uses: a listener that throws loses its
            // subscription rather than the log and a call every second.
            if(w->handle==handle) power_close_slot(ctx,i);
        }
        JS_FreeValue(ctx,result);
        JS_FreeValue(ctx,payload);
        JS_FreeValue(ctx,fn);
    }
}

// ----------------------------------------------------- unimplemented surfaces

static JSValue js_unsupported(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int magic,
                              JSValueConst *func_data) {
    (void)this_val; (void)argc; (void)argv; (void)magic;
    const char *operation=JS_ToCString(ctx,func_data[0]);
    JSValue error=reject(ctx,POCKET_ERR_UNSUPPORTED,operation?operation:"audio",
                         "not implemented in this build",false,
                         POCKET_OUTCOME_NOT_APPLIED);
    if(operation) JS_FreeCString(ctx,operation);
    return error;
}

// Section 2 asks a supported=false feature to keep its namespace and method and
// to fail the call with UNSUPPORTED, so audio.capture and audio.playback are
// here as names that reject rather than as a TypeError about undefined.
static void add_unsupported(JSContext *ctx, JSValue parent, const char *child,
                            const char *method, const char *operation) {
    JSValue object=JS_NewObject(ctx);
    JSValue name=JS_NewString(ctx,operation);
    JS_DefinePropertyValueStr(ctx,object,method,
        JS_NewCFunctionData(ctx,js_unsupported,2,0,1,&name),JS_PROP_ENUMERABLE);
    JS_FreeValue(ctx,name);
    JS_DefinePropertyValueStr(ctx,parent,child,object,JS_PROP_ENUMERABLE);
}

// ------------------------------------------------------------- pump / reset

void pocket_av_pump(void) {
    if(!av_ctx) return;
    if(tone.active) tone_pump(av_ctx);
    if(power_open) power_pump(av_ctx);
}

void pocket_av_reset(void) {
    if(av_ctx) {
        if(tone.active) {
            // The realm is going away, so there is nobody left to settle to.
            // The tone is asked to stop and then left alone: its completion
            // carries a request number this session will never read again, and
            // the audio task holds no JS value to free.
            sound_tone_cancel(tone.id);
            tone_release(av_ctx);
        }
        for(int i=0;i<POWER_WATCHES;i++) power_close_slot(av_ctx,i);
    }
    tone.active=false;
    tone.stop_code=NULL;
    power_open=0;
    power_primed=false;
    power_next_us=0;
    av_ctx=NULL;
}

// ------------------------------------------------------------- capabilities

static const pocket_limit_t cue_limits[] = {
    {.name="names", .kind=POCKET_LIMIT_TEXT, .text="move,accept,back"},
    {0},
};
static const pocket_limit_t tone_limits[] = {
    {.name="minFrequencyHz",.kind=POCKET_LIMIT_INT,.number=SOUND_TONE_MIN_HZ},
    {.name="maxFrequencyHz",.kind=POCKET_LIMIT_INT,.number=SOUND_TONE_MAX_HZ},
    {.name="maxDurationMs", .kind=POCKET_LIMIT_INT,.number=SOUND_TONE_MAX_MS},
    {.name="maxTimeoutMs",  .kind=POCKET_LIMIT_INT,.number=AV_MAX_TIMEOUT_MS},
    {.name="concurrent",    .kind=POCKET_LIMIT_INT,.number=1},
    {0},
};
static const pocket_limit_t power_limits[] = {
    {.name="millivolts",.kind=POCKET_LIMIT_FLAG,.number=1},
    {.name="percent",   .kind=POCKET_LIMIT_FLAG,.number=0},  // no cell curve
    {.name="charging",  .kind=POCKET_LIMIT_FLAG,.number=0},  // CHRG reaches the LED only
    {.name="keepAwake", .kind=POCKET_LIMIT_FLAG,.number=0},  // nothing sleeps yet
    {.name="maxWatches",.kind=POCKET_LIMIT_INT, .number=POWER_WATCHES},
    {.name="pollMs",    .kind=POCKET_LIMIT_INT, .number=POWER_POLL_US/1000},
    {0},
};

// available is the codec being there, not the mute setting: section 9 has a
// muted tone keep its full duration in silence, so muting changes what is heard
// and not whether the call works.
static void audio_probe(const pocket_capability_t *cap, bool *available,
                        const char **reason) {
    (void)cap;
    *available=sound_available();
    *reason=*available?NULL:POCKET_REASON_NO_DEVICE;
}

static void power_probe(const pocket_capability_t *cap, bool *available,
                        const char **reason) {
    (void)cap;
    board_battery_t battery;
    *available=board_battery_read(&battery);
    *reason=*available?NULL:POCKET_REASON_NO_DEVICE;
}

// audio.cue and power are not in section 2's list, which gives examples and
// leaves unlisted names to return supported=false. Both are named here because
// a program has to be able to detect them: cue is a stage A feature with no
// name of its own, and power is where the two nulls above are documented.
// audio.capture and audio.playback keep pocket_api.c's declared entries, which
// already say supported=false with NOT_IMPLEMENTED.
static const pocket_capability_t audio_cue_capability = {
    .name="audio.cue", .supported=true, .available=false,
    .reason=POCKET_REASON_NO_DEVICE, .limits=cue_limits, .probe=audio_probe,
};
static const pocket_capability_t audio_tone_capability = {
    .name="audio.tone", .supported=true, .available=false,
    .reason=POCKET_REASON_NO_DEVICE, .limits=tone_limits, .probe=audio_probe,
};
static const pocket_capability_t power_capability = {
    .name="power", .supported=true, .available=false,
    .reason=POCKET_REASON_NO_DEVICE, .limits=power_limits, .probe=power_probe,
};

esp_err_t pocket_av_install(JSContext *ctx, void *user_data) {
    (void)user_data;
    pocket_api_register(&audio_cue_capability);
    pocket_api_register(&audio_tone_capability);
    pocket_api_register(&power_capability);

    // A realm going away takes its callbacks with it, so the tables start empty
    // on every install. tone_next_request is not touched: it is the one piece
    // of state that has to outlive a session.
    tone.resolve=tone.reject=tone.cancel=JS_UNDEFINED;
    tone.active=false;
    tone.stop_code=NULL;
    for(int i=0;i<POWER_WATCHES;i++) {
        power_watches[i].callback=JS_UNDEFINED;
        power_watches[i].handle=0;
    }
    power_open=0;
    power_primed=false;
    av_ctx=ctx;

    JSValue root=pocket_api_root(ctx);
    if(JS_IsUndefined(root)) { JS_FreeValue(ctx,root); return ESP_ERR_INVALID_STATE; }

    JSValue audio=JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx,audio,"cue",
        JS_NewCFunction(ctx,js_cue,"cue",1),JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx,audio,"tone",
        JS_NewCFunction(ctx,js_tone,"tone",2),JS_PROP_ENUMERABLE);
    add_unsupported(ctx,audio,"capture","open","audio.capture.open");
    add_unsupported(ctx,audio,"player","open","audio.player.open");
    JS_DefinePropertyValueStr(ctx,root,"audio",audio,JS_PROP_ENUMERABLE);

    JSValue power=JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx,power,"status",
        JS_NewCFunction(ctx,js_status,"status",0),JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx,power,"onChange",
        JS_NewCFunction(ctx,js_on_change,"onChange",1),JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx,power,"keepAwake",
        JS_NewCFunction(ctx,js_keep_awake,"keepAwake",1),JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx,root,"power",power,JS_PROP_ENUMERABLE);

    JS_FreeValue(ctx,root);
    return ESP_OK;
}
