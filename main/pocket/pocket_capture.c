#include "pocket_capture.h"
#include "pocket_api.h"
#include "sound.h"
#include "board.h"
#include "paint.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Section 14 caps a general Promise at 30000ms and every other surface here
// enforces the same number. A read waits for the microphone rather than for a
// queue, so its default is short: at 24 kHz the ring holds 64 ms, and a program
// that has been waiting a second for a frame has a broken microphone, not a
// slow one.
#define CAPTURE_MAX_TIMEOUT_MS     30000
#define CAPTURE_DEFAULT_TIMEOUT_MS 1000

// How often a recording asks for a repaint it did not otherwise get. The
// indicator is composited into every strip that is presented, so this only
// covers the case of an app that has stopped drawing entirely -- twice a second
// is enough for the dot never to be missing for long, and cheap enough that a
// recording does not force 30 full repaints a second on its own.
#define CAPTURE_INDICATOR_US 500000

// ------------------------------------------------------------------- options
//
// The same two fields pocket_av.c's tone takes, and for the same reason: a read
// can wait, so its cancel token has to be polled for the whole wait rather than
// observed once at the call.

typedef struct {
    int32_t timeout_ms;    // 0 for "not given"
    JSValue cancel;        // JS_UNDEFINED when none
    bool    cancelled;     // the token was already cancelled at the call
} capture_options_t;

static JSValue take_options(JSContext *ctx, JSValueConst value,
                            const char *operation, capture_options_t *out) {
    out->timeout_ms=0;
    out->cancel=JS_UNDEFINED;
    out->cancelled=false;
    if(JS_IsUndefined(value) || JS_IsNull(value)) return JS_UNDEFINED;
    if(!JS_IsObject(value))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,operation,
                                 "options must be an object",false,
                                 POCKET_OUTCOME_NOT_APPLIED);

    JSValue timeout=JS_GetPropertyStr(ctx,value,"timeoutMs");
    if(JS_IsException(timeout)) return JS_EXCEPTION;
    if(!JS_IsUndefined(timeout)) {
        double ms=0;
        bool bad=!JS_IsNumber(timeout) || JS_ToFloat64(ctx,&ms,timeout);
        JS_FreeValue(ctx,timeout);
        if(bad || !isfinite(ms) || ms!=(double)(int64_t)ms ||
           ms<1 || ms>CAPTURE_MAX_TIMEOUT_MS)
            return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,operation,
                                     "timeoutMs must be a whole number of 1 to 30000",
                                     false,POCKET_OUTCOME_NOT_APPLIED);
        out->timeout_ms=(int32_t)ms;
    } else JS_FreeValue(ctx,timeout);

    JSValue cancel=JS_GetPropertyStr(ctx,value,"cancel");
    if(JS_IsException(cancel)) return JS_EXCEPTION;
    if(!JS_IsUndefined(cancel) && !JS_IsNull(cancel)) {
        if(!pocket_api_is_cancel_token(cancel)) {
            JS_FreeValue(ctx,cancel);
            return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,operation,
                                     "cancel must be a token from pocket.cancel.source()",
                                     false,POCKET_OUTCOME_NOT_APPLIED);
        }
        out->cancelled=pocket_api_cancel_requested(cancel);
        out->cancel=cancel;
        return JS_UNDEFINED;
    }
    JS_FreeValue(ctx,cancel);
    return JS_UNDEFINED;
}

// ------------------------------------------------------------- the recorder
//
// One recorder, because one microphone. Its identity is a number that is never
// reused, carried by every method it hands out, so a method from a closed
// recorder answers CLOSED instead of reaching a recording that is not its own.

static struct {
    bool             open;
    int32_t          id;
    uint32_t         delivered;    // frames handed to the app, for the log line
    // The read in flight. 0 when none: one read at a time, which is what makes
    // "time-contiguous" a property of the sequence and not just of one array.
    pocket_request_t request;
    int32_t          want;         // its maxFrames
    int16_t         *data;         // filled by the pump; settle() takes it
    int32_t          got;
} rec;

static int32_t next_id=1;

// What the pump posts. POCKET_STATUS_OK means `data` holds `got` frames.
enum {
    CAPTURE_STATUS_OVERFLOW = 1,   // the DMA wrapped onto unread audio
    CAPTURE_STATUS_IO       = 2,   // the driver refused the read
    CAPTURE_STATUS_STOPPED  = 3,   // timeout, cancel or a close under the read
};

// Closes the microphone and forgets the recorder. Safe to call twice, and the
// only place the HAL is given back.
static void teardown(void) {
    if(!rec.open) return;
    rec.open=false;
    ESP_LOGI("capture","CAPTURE CLOSE delivered=%u",(unsigned)rec.delivered);
    sound_capture_stop();
}

// An Int16Array of exactly `n` frames. The guest pays for what it keeps and
// nothing more; the host's copy is freed by the caller.
static JSValue frames_value(JSContext *ctx, const int16_t *pcm, int n) {
    JSValue length=JS_NewInt32(ctx,n);
    JSValue array=JS_NewTypedArray(ctx,1,(JSValueConst *)&length,
                                   JS_TYPED_ARRAY_INT16);
    JS_FreeValue(ctx,length);
    if(JS_IsException(array)) return array;
    size_t offset=0,bytes=0,per=0;
    JSValue buffer=JS_GetTypedArrayBuffer(ctx,array,&offset,&bytes,&per);
    if(JS_IsException(buffer)) { JS_FreeValue(ctx,array); return buffer; }
    size_t size=0;
    uint8_t *raw=JS_GetArrayBuffer(ctx,&size,buffer);
    JS_FreeValue(ctx,buffer);
    // A freshly built typed array is neither detached nor short; the check is
    // here because the alternative to checking is memcpy into whatever it is.
    if(!raw || offset+bytes>size || bytes!=(size_t)n*sizeof(int16_t)) {
        JS_FreeValue(ctx,array);
        return pocket_api_error(ctx,POCKET_ERR_OUT_OF_MEMORY,"audio.capture.read",
                                "no room for the samples",true,
                                POCKET_OUTCOME_NOT_APPLIED);
    }
    if(n) memcpy(raw+offset,pcm,bytes);
    return array;
}

// ---- the promise a waiting read holds

static JSValue read_settle(JSContext *ctx, void *user, int32_t status,
                           const char *stop_code, bool *rejected) {
    (void)user;
    static const char *const OP="audio.capture.read";
    int16_t *data=rec.data; int32_t got=rec.got;
    rec.data=NULL; rec.got=0; rec.want=0;
    *rejected=true;
    if(stop_code) {
        free(data);
        // Nothing was handed over, so the outcome is not in doubt: a read that
        // was stopped delivered no frames, and the ones the microphone produced
        // meanwhile are still in the ring for the next read.
        return pocket_api_error(ctx,stop_code,OP,
                                !strcmp(stop_code,POCKET_ERR_TIMEOUT)
                                    ?"no audio arrived before timeoutMs"
                                    :"the read was stopped",
                                true,POCKET_OUTCOME_NOT_APPLIED);
    }
    switch(status) {
        case POCKET_STATUS_OK: {
            JSValue value=frames_value(ctx,data,got);
            free(data);
            if(JS_IsException(value)) return value;
            rec.delivered+=(uint32_t)got;
            *rejected=false;
            return value;
        }
        case CAPTURE_STATUS_OVERFLOW:
            free(data);
            // Section 9: do not splice. The recorder is already closed by the
            // pump that saw this, so the app cannot read across the hole.
            return pocket_api_error(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                "the recording overran its buffer and was closed; read more often "
                "or ask for fewer frames",false,POCKET_OUTCOME_NOT_APPLIED);
        case CAPTURE_STATUS_STOPPED:
            free(data);
            // Posted by close() with a read still waiting. The substrate's own
            // stop path arrives as a stop_code above and never gets here, so
            // this is only ever the app closing its own recorder.
            return pocket_api_error(ctx,POCKET_ERR_CLOSED,OP,
                                    "the recorder was closed while the read waited",
                                    false,POCKET_OUTCOME_NOT_APPLIED);
        default:
            free(data);
            return pocket_api_error(ctx,POCKET_ERR_IO_ERROR,OP,
                                    "the microphone stopped delivering audio",
                                    true,POCKET_OUTCOME_UNKNOWN);
    }
}

// Timeout, a cancelled token, or a session ending. There is no driver to ask to
// stop -- the read is a poll of a ring -- so the completion the substrate is
// waiting for has to be posted here. On a session ending nobody reads it, which
// is exactly what an unmatched request number is for.
static void read_stop(void *user, const char *code) {
    (void)code;
    pocket_api_complete((pocket_request_t)(uintptr_t)user,CAPTURE_STATUS_STOPPED);
}

static void read_released(void *user) {
    (void)user;
    rec.request=0;
    free(rec.data);
    rec.data=NULL;
    rec.got=0;
}

static const pocket_promise_ops_t read_ops = {
    .settle=read_settle, .stop=read_stop, .release=read_released,
};

// ---- the methods

enum { M_READ=0, M_CLOSE };

static bool recorder_live(JSContext *ctx, JSValueConst id) {
    int32_t want=0;
    if(JS_ToInt32(ctx,&want,id)) return false;
    return rec.open && rec.id==want;
}

// One attempt at the ring. Returns the frame count, or a negative
// CAPTURE_STATUS_* the caller turns into a rejection. Never partial: what it
// writes is contiguous, because sound_capture_read() reports a wrapped ring
// rather than handing back what straddles it.
static int pull(int16_t *into, int32_t want) {
    int got=sound_capture_read(into,(int)want);
    if(got==-1) return -CAPTURE_STATUS_OVERFLOW;
    if(got<0)   return -CAPTURE_STATUS_IO;
    return got;
}

static JSValue js_recorder_method(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int magic,
                                  JSValueConst *func_data) {
    (void)this_val;
    static const char *const OPS[]={"audio.capture.read","audio.capture.close"};
    const char *op=OPS[magic];

    if(!recorder_live(ctx,func_data[0])) {
        // close() on a closed recorder is a no-op, as section 4 asks of every
        // close in this document.
        if(magic==M_CLOSE) return JS_UNDEFINED;
        return pocket_api_reject(ctx,POCKET_ERR_CLOSED,op,"the recorder is closed",
                                 false,POCKET_OUTCOME_NOT_APPLIED);
    }

    if(magic==M_CLOSE) {
        // A read still waiting is stopped, not orphaned: its promise settles
        // with CLOSED on the next pump.
        if(rec.request) pocket_api_complete(rec.request,CAPTURE_STATUS_STOPPED);
        teardown();
        return JS_UNDEFINED;
    }

    // ---- read(maxFrames, options)
    double frames=0;
    bool bad=argc<1 || !JS_IsNumber(argv[0]) || JS_ToFloat64(ctx,&frames,argv[0]);
    if(bad || !isfinite(frames) || frames!=(double)(int64_t)frames || frames<1)
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                 "read(maxFrames) needs a whole number of 1 or more",
                                 false,POCKET_OUTCOME_NOT_APPLIED);
    // Over the published ceiling is LIMIT_EXCEEDED and not INVALID_ARGUMENT, so
    // an app can read maxFrames out of limits and act on the difference. Section
    // 4 refuses to round an over-range request quietly, so neither is this
    // clamped.
    if(frames>SOUND_CAPTURE_MAX_FRAMES)
        return pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,op,
                                 "maxFrames is above maxReadFrames",false,
                                 POCKET_OUTCOME_NOT_APPLIED);

    capture_options_t options;
    JSValue refused=take_options(ctx,argc>1?argv[1]:JS_UNDEFINED,op,&options);
    if(!JS_IsUndefined(refused)) return refused;

    // Section 4: one operation of a kind per handle, and the second answers
    // BUSY. Two reads in flight would also be two claims on one contiguous
    // stream, with no order between them.
    if(rec.request) {
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,op,"a read is already waiting",
                                 true,POCKET_OUTCOME_NOT_APPLIED);
    }
    if(options.cancelled) {
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,op,
                                 "cancelled before the read",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }

    int32_t want=(int32_t)frames;
    int16_t *buffer=malloc((size_t)want*sizeof(int16_t));
    if(!buffer) {
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_reject(ctx,POCKET_ERR_OUT_OF_MEMORY,op,
                                 "no room for the samples",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    // Tried here rather than left to the pump, which runs on the next frame:
    // the ring holds 64 ms and the frame is 33, so a read that always cost a
    // frame of latency would halve the headroom every app has.
    int got=pull(buffer,want);
    if(got>0) {
        JS_FreeValue(ctx,options.cancel);
        JSValue value=frames_value(ctx,buffer,got);
        free(buffer);
        if(JS_IsException(value)) return value;
        rec.delivered+=(uint32_t)got;
        return pocket_api_settled(ctx,value,false);
    }
    if(got<0) {
        free(buffer);
        JS_FreeValue(ctx,options.cancel);
        bool overflow=got==-CAPTURE_STATUS_OVERFLOW;
        ESP_LOGW("capture","CAPTURE OVERFLOW frames=%u",(unsigned)rec.delivered);
        teardown();
        return pocket_api_reject(ctx,
            overflow?POCKET_ERR_LIMIT_EXCEEDED:POCKET_ERR_IO_ERROR,op,
            overflow?"the recording overran its buffer and was closed; read more "
                     "often or ask for fewer frames"
                    :"the microphone stopped delivering audio",
            !overflow,POCKET_OUTCOME_NOT_APPLIED);
    }

    // Nothing yet. Wait on the substrate's slot and let the pump finish it.
    free(buffer);
    pocket_request_t request=pocket_api_promise_open();
    if(!request) {
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,op,
                                 "too many operations are pending",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    rec.request=request; rec.want=want; rec.data=NULL; rec.got=0;
    int64_t deadline_us=esp_timer_get_time()+
        1000LL*(options.timeout_ms?options.timeout_ms:CAPTURE_DEFAULT_TIMEOUT_MS);
    JSValue promise=pocket_api_promise_arm(ctx,request,&read_ops,
                                           (void *)(uintptr_t)request,
                                           options.cancel,deadline_us);
    if(JS_IsException(promise)) { rec.request=0; rec.want=0; }
    return promise;
}

static void recorder_add(JSContext *ctx, JSValue object, const char *name,
                         int length, int magic, JSValueConst id) {
    JS_DefinePropertyValueStr(ctx,object,name,
        JS_NewCFunctionData(ctx,js_recorder_method,length,magic,1,&id),
        JS_PROP_ENUMERABLE);
}

// ---- open

// One integer field of the spec, checked against the single value this host
// enforces. Returns NULL when it is acceptable, else the message to refuse with.
static const char *want_exactly(JSContext *ctx, JSValueConst spec,
                                const char *field, int32_t only,
                                const char *message) {
    JSValue value=JS_GetPropertyStr(ctx,spec,field);
    if(JS_IsException(value)) return message;
    if(JS_IsUndefined(value) || JS_IsNull(value)) { JS_FreeValue(ctx,value); return NULL; }
    double n=0;
    bool bad=!JS_IsNumber(value) || JS_ToFloat64(ctx,&n,value);
    JS_FreeValue(ctx,value);
    if(bad || n!=(double)only) return message;
    return NULL;
}

static JSValue js_open(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv) {
    (void)this_val;
    static const char *const OP="audio.capture.open";
    if(argc<1 || !JS_IsObject(argv[0]))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "open(spec, options) needs a spec object",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(!sound_available())
        return pocket_api_reject(ctx,POCKET_ERR_NOT_AVAILABLE,OP,
                                 "no audio codec on this unit",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(rec.open)
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
                                 "a recorder is already open",true,
                                 POCKET_OUTCOME_NOT_APPLIED);

    // Section 9's type block says 16000. This host records at the rate the DAC
    // already runs at, because the microphone arrives through the same I2S
    // controller and one controller has one clock -- see main/hal/sound.h. A
    // rate this host cannot produce is refused rather than silently substituted,
    // the same way audio.player refuses a WAV that is not 24 kHz.
    const char *why=want_exactly(ctx,argv[0],"sampleRate",(int32_t)SOUND_SAMPLE_RATE,
                                 "this host records at 24000 Hz and has no resampler");
    if(!why) why=want_exactly(ctx,argv[0],"channels",1,
                              "this host records one channel");
    if(why)
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,why,false,
                                 POCKET_OUTCOME_NOT_APPLIED);

    capture_options_t options;
    JSValue refused=take_options(ctx,argc>1?argv[1]:JS_UNDEFINED,OP,&options);
    if(!JS_IsUndefined(refused)) return refused;
    bool cancelled=options.cancelled;
    JS_FreeValue(ctx,options.cancel);   // open is over before it could be polled
    if(cancelled)
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,
                                 "cancelled before the recorder opened",false,
                                 POCKET_OUTCOME_NOT_APPLIED);

    // The exclusion section 9 asks for. sound.c refuses while anything is
    // queued or sounding, so a tone or a clip that is still playing means this
    // call is early -- BUSY and retryable, not a refusal of the feature.
    if(!sound_capture_start())
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
                                 "the speaker is busy; recording and playback are "
                                 "exclusive on this unit",true,
                                 POCKET_OUTCOME_NOT_APPLIED);

    rec.open=true; rec.id=next_id++;
    rec.delivered=0; rec.request=0; rec.want=0; rec.data=NULL; rec.got=0;

    JSValue object=JS_NewObject(ctx);
    if(JS_IsException(object)) { teardown(); return object; }
    JSValue key=JS_NewInt32(ctx,rec.id);
    recorder_add(ctx,object,"read",2,M_READ,key);
    recorder_add(ctx,object,"close",0,M_CLOSE,key);
    JS_FreeValue(ctx,key);
    return pocket_api_settled(ctx,object,false);
}

// ------------------------------------------------------------- pump / reset

void pocket_capture_pump(void) {
    if(!rec.request || !rec.open) return;
    if(rec.data) return;                  // already filled, waiting to settle
    int16_t *buffer=malloc((size_t)rec.want*sizeof(int16_t));
    if(!buffer) return;                   // try again next turn; the deadline
                                          // is what ends a read that never can
    int got=pull(buffer,rec.want);
    if(got==0) { free(buffer); return; }
    if(got<0) {
        free(buffer);
        if(got==-CAPTURE_STATUS_OVERFLOW)
            ESP_LOGW("capture","CAPTURE OVERFLOW frames=%u",(unsigned)rec.delivered);
        pocket_api_complete(rec.request,
                            got==-CAPTURE_STATUS_OVERFLOW?CAPTURE_STATUS_OVERFLOW
                                                         :CAPTURE_STATUS_IO);
        // Closed before the app is told, so nothing can read across the hole
        // even from inside the rejection handler.
        teardown();
        return;
    }
    rec.data=buffer; rec.got=got;
    pocket_api_complete(rec.request,POCKET_STATUS_OK);
}

void pocket_capture_reset(void) {
    // The promise slot is pocket_api_reset()'s to release; what is here is the
    // microphone, which nothing else in the firmware would give back.
    teardown();
    rec.request=0;
    free(rec.data);
    rec.data=NULL; rec.got=0; rec.want=0;
}

// ---------------------------------------------------------------- indicator

static int64_t indicator_next_us;

bool pocket_capture_take_dirty(void) {
    if(!sound_capture_active()) return false;
    int64_t now=esp_timer_get_time();
    if(now<indicator_next_us) return false;
    indicator_next_us=now+CAPTURE_INDICATOR_US;
    return true;
}

// A red dot in the top-right corner, on a dark pad so it reads over whatever is
// behind it. Drawn from board_present(), which is the only transfer to the
// panel, so an app cannot paint over it: whatever it drew has already been
// overwritten here by the time the strip leaves.
//
// That is only half of why the indicator is correct, and the other half is
// pocket_capture_take_dirty() above -- NOT this hook. board_present() is the
// only path to the panel, but a frame that presents three strips updates three
// strips: a screen nobody is repainting is a screen with no dot on it, and this
// function is never called for a strip nobody sends. The editor's differential
// redraw broke exactly this assumption elsewhere in the firmware, and
// board_capture() cannot see it, because what is wrong is what was never sent.
// If the repaint request is ever removed, the indicator stops being a guarantee
// and becomes a side effect of the app choosing to draw.
void pocket_capture_overlay(uint16_t *pixels, int y, int rows) {
    if(!sound_capture_active()) return;
    // The dot, and a level meter beside it.
    //
    // The dot is the API's obligation: it means "the microphone is live" and
    // nothing else, so it keeps its size, its position and its colour. Red is
    // spoken for by it -- the meter beside it is neutral, and the ONE thing
    // that changes colour is a clip, so a change of colour on this corner
    // always means something is wrong rather than something is happening.
    //
    // The meter answers the two questions a person has while they are the one
    // making the sound: is it clipping, and is it far too quiet. It is a column
    // of six cells so those are legible at a glance rather than a bar whose
    // exact height has to be judged: an empty column is too quiet, a full one
    // with the top cell amber is clipping.
    // Geometry, and it has to be checked rather than eyeballed: the meter's
    // cells stack upward from the bottom of the dot, and a first attempt at six
    // three-pixel cells ran off the top of the panel where nothing would have
    // drawn them. The pad is the bounding box of both and everything else is
    // measured from it.
    enum { PAD_X=216, PAD_Y=0, PAD_W=24, PAD_H=15,
           DOT_X=226, DOT_Y=4, DOT=8,
           BAR_X=219, BAR_W=4, CELLS=6, CELL=2, BAR_BOTTOM=12 };
    if(y>=PAD_Y+PAD_H || y+rows<=PAD_Y) return;
    paint_begin(pixels,y,rows);
    paint_fill(PAD_X,PAD_Y,PAD_W,PAD_H,board_rgb(16,8,8));
    paint_fill(DOT_X,DOT_Y,DOT,DOT,board_rgb(232,48,48));

    unsigned peak=0;
    bool clipping=false;
    sound_capture_level(&peak,&clipping);
    // Cells are doublings, not a linear scale: the ear is logarithmic and so is
    // the question being asked. The lowest cell lights at about 1/64 of full
    // scale, which is around where a recording stops being usable, so "no cells
    // while somebody is talking" is the too-quiet signal.
    int lit=0;
    for(unsigned threshold=32767u>>CELLS; lit<CELLS && peak>=threshold; threshold<<=1)
        lit++;
    for(int c=0;c<CELLS;c++) {
        int top=BAR_BOTTOM-(c+1)*CELL;      // c=0 is the bottom cell
        bool on=c<lit;
        // Amber only for the clip, and only on the cell that means "the top".
        uint16_t colour = on ? ((clipping && c==CELLS-1) ? board_rgb(255,176,0)
                                                        : board_rgb(210,222,230))
                             : board_rgb(40,36,36);
        paint_fill(BAR_X,top,BAR_W,CELL-1,colour);   // -1 leaves a gap between cells
    }
}


// ------------------------------------------------------------- capability

// Section 2: publish what the code enforces, and nothing else. Every number
// here is checked in this file or in sound.c.
static const pocket_limit_t capture_limits[] = {
    {.name="sampleRate",    .kind=POCKET_LIMIT_INT,.number=(int32_t)SOUND_SAMPLE_RATE},
    {.name="channels",      .kind=POCKET_LIMIT_INT,.number=1},
    {.name="maxReadFrames", .kind=POCKET_LIMIT_INT,.number=SOUND_CAPTURE_MAX_FRAMES},
    {.name="maxTimeoutMs",  .kind=POCKET_LIMIT_INT,.number=CAPTURE_MAX_TIMEOUT_MS},
    {.name="concurrent",    .kind=POCKET_LIMIT_INT,.number=1},
    // Both of these are the same fact from the two sides an app cares about:
    // it may not play while recording, and there is no second stream to mix.
    {.name="fullDuplex",    .kind=POCKET_LIMIT_FLAG,.number=0},
    {.name="playsWhileRecording",.kind=POCKET_LIMIT_FLAG,.number=0},
    {0},
};

// available is the codec being there. Muting is not consulted: mute is about
// what leaves the speaker, and section 9 keeps recording separate from it.
static void capture_probe(const pocket_capability_t *cap, bool *available,
                          const char **reason) {
    (void)cap;
    *available=sound_available();
    *reason=*available?NULL:POCKET_REASON_NO_DEVICE;
}

static const pocket_capability_t capture_capability = {
    .name="audio.capture", .supported=true, .available=false,
    .reason=POCKET_REASON_NO_DEVICE, .limits=capture_limits, .probe=capture_probe,
};

static esp_err_t build_capture(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    // A realm going away takes the recorder's methods with it, so a namespace
    // being built is also the moment nothing can be holding one.
    rec.open=false; rec.request=0;
    free(rec.data); rec.data=NULL; rec.got=0; rec.want=0;
    JSValue object=JS_NewObject(ctx);
    if(JS_IsException(object)) return ESP_ERR_NO_MEM;
    JS_DefinePropertyValueStr(ctx,object,"open",
        JS_NewCFunction(ctx,js_open,"open",2),JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx,ns,"capture",object,JS_PROP_ENUMERABLE);
    return ESP_OK;
}

esp_err_t pocket_capture_install(JSContext *ctx, void *user_data) {
    (void)user_data;
    pocket_api_register(&capture_capability);
    // A second contributor to the audio namespace pocket_av.c also builds. The
    // substrate runs them in registration order and neither knows about the
    // other, which is what keeps the microphone out of a file that links
    // against the filesystem.
    return pocket_api_lazy(ctx,"audio",build_capture,NULL);
}
