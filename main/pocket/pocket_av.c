#include "pocket_av.h"
#include "pocket_api.h"
#include "pocket_fs.h"
#include "sound.h"
#include "board.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

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

// ------------------------------------------------------------------- options
//
// Section 4 puts argument errors from a Promise-returning method into the
// rejection rather than a throw, so the exits below go through
// pocket_api_reject().

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
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,operation,
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
// posts a completion and stops. pocket_api_pump() reads it on the JS task and
// is the only code that settles the Promise.
//
// The request number, not a flag, is what the two sides agree on: sound_tone()
// may finish a 1ms tone before it has even returned its id, and a session may
// end with a tone still sounding. Matching numbers makes both harmless -- a
// completion whose number nobody is waiting for is simply never read. That
// number and the rule behind it now belong to pocket_api.c, which is also where
// the reason its counter is never reset is written down.

// The only status audio.tone posts besides POCKET_STATUS_OK. Nobody asked the
// tone to stop, so the I2S write failed under it.
#define TONE_STATUS_STOPPED_EARLY 1

static struct {
    bool    active;
    int32_t id;         // sound.c's id, for sound_tone_cancel
} tone;

static void tone_done(void *ctx, bool completed) {
    pocket_api_complete((pocket_request_t)(uintptr_t)ctx,
                        completed?POCKET_STATUS_OK:TONE_STATUS_STOPPED_EARLY);
}

// Asks the tone to stop. The Promise is not settled here: section 4 gives the
// host the wait for the native stop, and the audio task still owns the request
// until its callback lands.
static void tone_stop(void *user, const char *code) {
    (void)user; (void)code;
    sound_tone_cancel(tone.id);
}

static JSValue tone_finish(JSContext *ctx, void *user, int32_t status,
                           const char *stop_code, bool *rejected) {
    (void)user;
    bool completed=status==POCKET_STATUS_OK;
    *rejected=true;
    if(stop_code)
        // The tone was told to stop. outcome is the honest part: a stop that
        // arrived after the last frame still made the sound.
        return pocket_api_error(ctx,stop_code,"audio.tone",
                                !strcmp(stop_code,POCKET_ERR_TIMEOUT)
                                    ?"the tone outlived timeoutMs"
                                    :"cancelled while playing",
                                false,
                                completed?POCKET_OUTCOME_APPLIED
                                         :POCKET_OUTCOME_UNKNOWN);
    if(completed) { *rejected=false; return JS_UNDEFINED; }
    return pocket_api_error(ctx,POCKET_ERR_IO_ERROR,"audio.tone",
                            "the tone stopped early",true,
                            POCKET_OUTCOME_UNKNOWN);
}

static void tone_released(void *user) {
    (void)user;
    tone.active=false;
}

static const pocket_promise_ops_t tone_ops = {
    .settle=tone_finish, .stop=tone_stop, .release=tone_released,
};

static JSValue js_tone(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv) {
    (void)this_val;
    static const char *const OP="audio.tone";
    if(argc<1 || !JS_IsObject(argv[0]))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
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
            return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                     "frequencyHz, durationMs and gain must be numbers",
                                     false,POCKET_OUTCOME_NOT_APPLIED);
    }
    // The same range sound.c enforces, checked here so the refusal arrives as a
    // PocketError with the limit in it rather than as SOUND_ERR_INVALID. The
    // rounding below is the 1Hz and 1ms resolution of the synthesiser, not a
    // silent clamp: a value outside the range is refused, never rounded into it.
    if(frequency<SOUND_TONE_MIN_HZ || frequency>SOUND_TONE_MAX_HZ)
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "frequencyHz must be 20 to 8000",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(duration<1 || duration>SOUND_TONE_MAX_MS)
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "durationMs must be 1 to 5000",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    // Written as a positive test so a NaN gain cannot pass two false compares.
    if(!(gain>=0.0 && gain<=1.0))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "gain must be 0 to 1",false,POCKET_OUTCOME_NOT_APPLIED);

    av_options_t options;
    JSValue bad=take_options(ctx,argc>1?argv[1]:JS_UNDEFINED,OP,&options);
    if(!JS_IsUndefined(bad)) return bad;

    // Section 4 allows one operation of a kind per handle and answers the
    // second with BUSY. One tone at a time is also what sound.c can cancel:
    // it remembers a single id.
    if(tone.active) {
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,"a tone is already playing",
                                 true,POCKET_OUTCOME_NOT_APPLIED);
    }
    if(options.cancelled) {
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,
                                 "cancelled before the tone",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }

    unsigned hz=(unsigned)(frequency+0.5), ms=(unsigned)(duration+0.5);
    // The slot is claimed before the audio task is given anything to finish, so
    // a 1ms tone that completes inside sound_tone() has somewhere to post to.
    // Unreachable while tone.active is the one tone at a time, but the table is
    // shared with every other surface that waits on a driver.
    pocket_request_t request=pocket_api_promise_open();
    if(!request) {
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
                                 "too many operations are pending",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    int32_t id=sound_tone(hz,ms,(float)gain,tone_done,(void *)(uintptr_t)request);
    if(id<0) {
        pocket_api_promise_abandon(request);
        JS_FreeValue(ctx,options.cancel);
        switch(id) {
            case SOUND_ERR_BUSY:
                return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
                                         "the sound queue is full",true,
                                         POCKET_OUTCOME_NOT_APPLIED);
            // sound.c calls this one UNSUPPORTED because it means "no codec on
            // this board", but section 2 reserves UNSUPPORTED for what the
            // firmware does not implement. A missing codec is NOT_AVAILABLE.
            case SOUND_ERR_UNSUPPORTED:
                return pocket_api_reject(ctx,POCKET_ERR_NOT_AVAILABLE,OP,
                                         "no audio codec on this unit",false,
                                         POCKET_OUTCOME_NOT_APPLIED);
            default:
                return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                         "the tone was refused",false,
                                         POCKET_OUTCOME_NOT_APPLIED);
        }
    }
    tone.id=id;
    // Section 4 measures timeoutMs from the call and includes the queue wait,
    // so the default has to cover a tone already sounding ahead of this one.
    int64_t deadline_us=esp_timer_get_time()+
        1000LL*(options.timeout_ms?options.timeout_ms:(int32_t)ms+AV_QUEUE_SLACK_MS);
    JSValue promise=pocket_api_promise_arm(ctx,request,&tone_ops,NULL,
                                           options.cancel,deadline_us);
    if(JS_IsException(promise)) {
        // Nothing can settle a Promise that was not built, so the tone is asked
        // to stop; its completion goes unread, which is what an unmatched
        // request number is for. The slot and the cancel token are already back.
        sound_tone_cancel(id);
        return promise;
    }
    tone.active=true;
    return promise;
}

// ------------------------------------------------------------ audio.player
//
// Section 9.1 asks for MP3, Opus or FLAC off sd:. None of that is reachable on
// this board and the numbers are in the section itself: IDF v6.0.1 ships no
// decoder at all, Espressif's own figures put a decoder's heap at 26.6 to 89.4
// KB against the 23,552 bytes that is the largest contiguous block measured
// while an app is up, sd: has no driver here, and app:/ caps a file at 24,576
// bytes -- 1.5 seconds of 128 kbps MP3, which is not a music player either.
//
// What is here instead is the same Player, bounded to what fits: one clip, in
// RAM, in the host's own 24 kHz mono, as IMA ADPCM or PCM16 inside a WAV. The
// bytes are read once at open and decoded in place by the audio task, so the
// whole feature is one allocation the app can see the size of, no ring, no
// second task and no second I2S channel. seek is real because the clip is in
// RAM; at ADPCM's block granularity, which limits publishes.

// The whole file, header included, and now exactly what app:/ allows.
//
// This was 8,192 for a reason that has expired. The largest contiguous block
// with an app running measured 23,552 bytes, and a clip that only loads when
// the heap is unfragmented is a clip that fails in front of the person using
// it -- so the cap was set well below the block rather than at the file limit.
// That block now measures 73,728 (tools/memlog.py --port --check), because
// static DIRAM went from 197,847 to 111,383 bytes. A 24,576-byte allocation
// against 73,728 is the same kind of margin 8,192 had against 23,552.
//
// It costs nothing but a constant and it triples every clip: 2.05 s of ADPCM
// instead of 0.68, 0.51 s of PCM16 instead of 0.17. The cap is now the file
// system's, so this stops being a second limit an app has to discover.
#define PLAYER_MAX_BYTES 24576
#define PLAYER_WATCHES   2
#define PLAYER_CODEC_PCM  "wav/pcm16"
#define PLAYER_CODEC_IMA  "wav/ima-adpcm"

typedef enum { P_READY=0, P_PLAYING, P_PAUSED, P_ENDED, P_ERROR } player_state_t;
static const char *const PLAYER_STATE_NAME[]={
    "ready","playing","paused","ended","error" };

static struct {
    bool     open;
    int32_t  id;            // identity the bound methods carry; never reused
    uint8_t *file;          // the whole file, one allocation
    uint32_t offset, bytes; // the data chunk within it
    uint16_t block;         // ADPCM block size, 0 for PCM16
    uint32_t per_block;     // output frames one ADPCM block is worth
    uint32_t frames;        // the clip's length in output frames
    uint32_t position;      // frames consumed before the running clip started
    int32_t  clip;          // sound.c's id, 0 when nothing is queued
    player_state_t state;
    bool     announce;      // a state change the pump has still to deliver
} player;

static int32_t player_next_id=1;

// The audio task's hand-back. Same rule as the tone's: post and stop.
//
// What the callback carries is a launch number, not sound.c's clip id: a clip
// short enough can be over before sound_clip_start() has returned that id, and
// a completion nobody can name yet is a completion that gets applied to the
// wrong thing. The number is minted before the launch and moved on by every
// halt, so a stopped clip's completion matches nothing and is dropped -- the
// same trick, and the same reason, as pocket_api.c's request numbers.
static uint32_t player_seq;
static atomic_int player_done_seq;
static atomic_bool player_done_ok;

static void clip_done(void *ctx, bool completed) {
    atomic_store(&player_done_ok,completed);
    atomic_store(&player_done_seq,(int)(intptr_t)ctx);
}

// ---- the container
//
// Every length below is checked against what is left of the file before it is
// used, so a truncated or lying header ends as a refusal rather than as a read
// past the buffer. Section 9.1 asks for bounds on metadata and blocks; the
// bound here is the whole file, which is smaller than any of them.
static const char *wav_parse(const uint8_t *f, uint32_t n) {
    if(n<44||memcmp(f,"RIFF",4)||memcmp(f+8,"WAVE",4)) return "not a WAV file";
    uint32_t at=12, rate=0, per_block=0;
    uint16_t format=0, channels=0, bits=0, align=0;
    bool have_fmt=false;
    player.offset=player.bytes=0;
    while(at+8<=n) {
        uint32_t size=(uint32_t)f[at+4]|((uint32_t)f[at+5]<<8)|
                      ((uint32_t)f[at+6]<<16)|((uint32_t)f[at+7]<<24);
        const uint8_t *body=f+at+8;
        if(size>n-at-8) return "a chunk runs past the end of the file";
        if(!memcmp(f+at,"fmt ",4)&&size>=16) {
            format=(uint16_t)(body[0]|(body[1]<<8));
            channels=(uint16_t)(body[2]|(body[3]<<8));
            rate=(uint32_t)body[4]|((uint32_t)body[5]<<8)|
                 ((uint32_t)body[6]<<16)|((uint32_t)body[7]<<24);
            align=(uint16_t)(body[12]|(body[13]<<8));
            bits=(uint16_t)(body[14]|(body[15]<<8));
            // IMA carries its samples-per-block in the fmt extension. Trusting
            // it rather than deriving it is what lets a file made by a tool
            // that pads its blocks still decode where its blocks really begin.
            if(size>=20) per_block=(uint32_t)(body[18]|(body[19]<<8));
            have_fmt=true;
        } else if(!memcmp(f+at,"data",4)) {
            player.offset=at+8; player.bytes=size;
        }
        at+=8+size+(size&1);    // chunks are padded to an even length
    }
    if(!have_fmt) return "the file has no fmt chunk";
    if(!player.bytes) return "the file has no audio in it";
    if(channels!=1) return "this host plays one channel";
    if(rate!=SOUND_SAMPLE_RATE) return "this host plays 24000 Hz and has no resampler";
    if(format==1&&bits==16) {
        player.block=0; player.per_block=1;
        player.frames=player.bytes/2;
    } else if(format==0x11&&bits==4) {
        if(align<8||(align&1)||align>player.bytes) return "the ADPCM block size is not usable";
        player.block=align;
        player.per_block=per_block?per_block:(uint32_t)(align-4)*2+1;
        uint32_t blocks=player.bytes/align, tail=player.bytes%align;
        player.frames=blocks*player.per_block;
        // A short last block is still worth its header sample and its nibbles.
        if(tail>=4) player.frames+=1+(tail-4)*2;
    } else return "the codec is not one this host decodes";
    if(!player.frames) return "the file has no audio in it";
    return NULL;
}

// ---- state

static void player_set_state(player_state_t state) {
    if(player.state==state) return;
    player.state=state;
    player.announce=true;   // delivered from the pump, never inside a JS call
}

// sound_clip_position() answers 0 both before the audio task has reached the
// clip and after it is over, and the pump is what tells those apart -- so
// between the last sample and the next frame the raw sum would walk backwards
// to where play() started. Keeping the highest reading is enough: within one
// clip the position only ever grows, and a halt or a seek sets it afresh.
static uint32_t player_reported;

static uint32_t player_frames_now(void) {
    if(player.state!=P_PLAYING||!player.clip) return player.position;
    uint32_t done=player.position+sound_clip_position(player.clip);
    if(done>player.frames) done=player.frames;
    if(done<player_reported) return player_reported;
    player_reported=done;
    return done;
}

// Stops whatever is sounding and takes the position with it. The read has to
// come first: sound_clip_position() answers 0 once the clip is over, and the
// stop is what makes it over. The few frames the audio task may add between the
// two are lost, which is a position up to 5ms behind and never ahead.
static bool player_halt(void) {
    if(!player.clip) return true;
    player.position=player_frames_now();
    bool released=sound_clip_stop(player.clip);
    player.clip=0;
    player_seq++;   // the completion this stop provokes now matches nothing
    return released;
}

// Queues the rest of the clip from `player.position`. ADPCM is only resumable
// where a block begins -- a block reseeds the decoder, mid-block there is
// nothing to reseed from -- so the position moves back to the start of the
// block it lands in. That rounding is the whole of seekResolutionMs.
static const char *player_launch(void) {
    uint32_t start=player.position, byte=player.offset;
    if(player.block) {
        uint32_t index=start/player.per_block;
        start=index*player.per_block;
        byte+=index*player.block;
    } else byte+=start*2;
    if(start>=player.frames||byte>=player.offset+player.bytes) return "ended";
    player.position=start;
    player_reported=start;
    uint32_t seq=++player_seq;
    int32_t id=sound_clip_start(player.file+byte,
                                player.offset+player.bytes-byte,
                                player.block?SOUND_CLIP_IMA:SOUND_CLIP_PCM16,
                                player.block,player.frames-start,1.0f,
                                clip_done,(void *)(uintptr_t)seq);
    if(id<0) { player_seq++; return id==SOUND_ERR_BUSY?"busy":"unavailable"; }
    player.clip=id;
    return NULL;
}

static void player_teardown(void) {
    if(!player.open) return;
    player.open=false;
    if(player_halt()) free(player.file);
    else ESP_LOGE("pocket.av","the audio task still holds a clip; its %u bytes stay",
                  (unsigned)player.bytes);
    player.file=NULL;
    player.state=P_READY; player.announce=false;
    player.position=0; player.frames=0; player.bytes=0;
}

// ---- onState

static pocket_sub_slot_t player_slots[PLAYER_WATCHES];
static pocket_sub_table_t player_table = {
    .slots=player_slots, .count=PLAYER_WATCHES,
    .tag="pocket.av", .what="onState", .close_on_throw=true,
};

static bool player_payload(JSContext *ctx, int slot, void *user, JSValue *payload) {
    (void)slot; (void)user;
    JSValue event=JS_NewObject(ctx);
    if(JS_IsException(event)) return false;
    JS_SetPropertyStr(ctx,event,"state",
                      JS_NewString(ctx,PLAYER_STATE_NAME[player.state]));
    // Nothing here fails asynchronously except an I2S write that stopped early,
    // and that is what P_ERROR is. A player that ends normally carries no error.
    JS_SetPropertyStr(ctx,event,"error",
        player.state==P_ERROR
            ?pocket_api_error(ctx,POCKET_ERR_IO_ERROR,"audio.player",
                              "the clip stopped early",true,
                              POCKET_OUTCOME_UNKNOWN)
            :JS_NULL);
    *payload=event;
    return true;
}

static void player_pump(void) {
    uint32_t done=(uint32_t)atomic_load(&player_done_seq);
    if(player.clip&&done==player_seq) {
        atomic_store(&player_done_seq,0);
        bool ok=atomic_load(&player_done_ok);
        player.clip=0;
        if(ok) player.position=player.frames;
        if(player.state==P_PLAYING) player_set_state(ok?P_ENDED:P_ERROR);
    }
    if(player.announce&&player_table.open) {
        player.announce=false;
        pocket_api_sub_deliver(&player_table,player_payload,NULL);
    } else player.announce=false;
}

// ---- the methods
//
// Bound the way pocket_io.c binds a handle's: the identity travels as the
// function's own data, so nothing about the player is reachable from JS except
// through a method it handed out, and a method from a closed player answers
// CLOSED rather than reaching a freed buffer.

enum { M_INFO=0, M_PLAY, M_PAUSE, M_SEEK, M_STATUS, M_ON_STATE, M_CLOSE };

// Whether the method's player is still the open one. Promise-returning methods
// reject with this; the synchronous ones throw it.
static bool player_live(JSContext *ctx, JSValueConst id) {
    int32_t want=0;
    if(JS_ToInt32(ctx,&want,id)) return false;
    return player.open&&player.id==want;
}

static JSValue js_player_method(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv, int magic,
                                JSValueConst *func_data) {
    (void)this_val;
    static const char *const OPS[]={
        "audio.player.info","audio.player.play","audio.player.pause",
        "audio.player.seek","audio.player.status","audio.player.onState",
        "audio.player.close" };
    const char *op=OPS[magic];
    bool promised=magic==M_PLAY||magic==M_PAUSE||magic==M_SEEK;
    if(!player_live(ctx,func_data[0])) {
        // close() on a closed player is a no-op, as section 4 asks of every
        // close(); everything else is CLOSED.
        if(magic==M_CLOSE) return JS_UNDEFINED;
        return promised
            ?pocket_api_reject(ctx,POCKET_ERR_CLOSED,op,"the player is closed",
                               false,POCKET_OUTCOME_NOT_APPLIED)
            :pocket_api_throw(ctx,POCKET_ERR_CLOSED,op,"the player is closed",
                              false,NULL);
    }
    switch(magic) {
        case M_INFO: {
            JSValue info=JS_NewObject(ctx);
            if(JS_IsException(info)) return info;
            JS_SetPropertyStr(ctx,info,"codec",
                JS_NewString(ctx,player.block?PLAYER_CODEC_IMA:PLAYER_CODEC_PCM));
            JS_SetPropertyStr(ctx,info,"sampleRate",JS_NewInt32(ctx,SOUND_SAMPLE_RATE));
            JS_SetPropertyStr(ctx,info,"channels",JS_NewInt32(ctx,1));
            JS_SetPropertyStr(ctx,info,"durationMs",
                JS_NewInt32(ctx,(int)(player.frames*1000u/SOUND_SAMPLE_RATE)));
            JS_SetPropertyStr(ctx,info,"seekable",JS_TRUE);
            return info;
        }
        case M_PLAY: {
            if(player.state==P_PLAYING) return pocket_api_settled(ctx,JS_UNDEFINED,false);
            // Section 9.1: an ended player is not replayable, and says so with
            // NOT_AVAILABLE rather than pretending to be closed.
            if(player.state==P_ENDED||player.state==P_ERROR)
                return pocket_api_reject(ctx,POCKET_ERR_NOT_AVAILABLE,op,
                    "this player is finished; open the source again",false,
                    POCKET_OUTCOME_NOT_APPLIED);
            const char *why=player_launch();
            if(why) {
                if(!strcmp(why,"ended")) {
                    player.position=player.frames;
                    player_set_state(P_ENDED);
                    return pocket_api_reject(ctx,POCKET_ERR_NOT_AVAILABLE,op,
                        "there is nothing left to play",false,
                        POCKET_OUTCOME_NOT_APPLIED);
                }
                return pocket_api_reject(ctx,
                    !strcmp(why,"busy")?POCKET_ERR_BUSY:POCKET_ERR_NOT_AVAILABLE,
                    op,!strcmp(why,"busy")?"the sound queue is full"
                                          :"no audio codec on this unit",
                    !strcmp(why,"busy"),POCKET_OUTCOME_NOT_APPLIED);
            }
            player_set_state(P_PLAYING);
            // Section 9.1 has play() resolve on the output being accepted, not
            // on the clip being over; that is what onState is for.
            return pocket_api_settled(ctx,JS_UNDEFINED,false);
        }
        case M_PAUSE: {
            if(player.state==P_PLAYING) {
                player_halt();
                player_set_state(P_PAUSED);
            }
            // Pausing a player that is ready, already paused or finished holds
            // the position it already has, which is nothing to do.
            return pocket_api_settled(ctx,JS_UNDEFINED,false);
        }
        case M_SEEK: {
            double ms=0;
            if(argc<1||!JS_IsNumber(argv[0])||JS_ToFloat64(ctx,&ms,argv[0])||
               !isfinite(ms)||ms<0)
                return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                    "seek(positionMs) needs a whole number of milliseconds from 0",
                    false,POCKET_OUTCOME_NOT_APPLIED);
            if(player.state==P_ENDED||player.state==P_ERROR)
                return pocket_api_reject(ctx,POCKET_ERR_NOT_AVAILABLE,op,
                    "this player is finished; open the source again",false,
                    POCKET_OUTCOME_NOT_APPLIED);
            uint32_t frame=(uint32_t)(ms*SOUND_SAMPLE_RATE/1000.0);
            if(frame>=player.frames)
                return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                    "positionMs is past the end of the clip",false,
                    POCKET_OUTCOME_NOT_APPLIED);
            bool was_playing=player.state==P_PLAYING;
            player_halt();
            player.position=frame; player_reported=frame;
            if(was_playing&&player_launch())
                // The clip could not be requeued, so the position moved but the
                // sound stopped. outcome=applied is the honest half of that.
                { player_set_state(P_PAUSED);
                  return pocket_api_reject(ctx,POCKET_ERR_NOT_AVAILABLE,op,
                      "the seek landed but playback could not resume",true,
                      POCKET_OUTCOME_APPLIED); }
            return pocket_api_settled(ctx,JS_UNDEFINED,false);
        }
        case M_STATUS: {
            JSValue status=JS_NewObject(ctx);
            if(JS_IsException(status)) return status;
            JS_SetPropertyStr(ctx,status,"state",
                JS_NewString(ctx,PLAYER_STATE_NAME[player.state]));
            JS_SetPropertyStr(ctx,status,"positionMs",
                JS_NewInt32(ctx,(int)(player_frames_now()*1000u/SOUND_SAMPLE_RATE)));
            // Always zero, and honestly so: the clip is in RAM before it starts,
            // so there is no producer to fall behind the I2S write. The field
            // stays because a streamed source would have one.
            JS_SetPropertyStr(ctx,status,"underruns",JS_NewInt32(ctx,0));
            return status;
        }
        case M_ON_STATE: {
            if(argc<1||!JS_IsFunction(ctx,argv[0]))
                return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                    "onState(listener) needs a function",false,NULL);
            return pocket_api_sub_open(ctx,&player_table,argv[0],op,
                                       "too many subscriptions",NULL);
        }
        default:
            player_teardown();
            pocket_api_sub_close_all(&player_table);
            return JS_UNDEFINED;
    }
}

static void player_add(JSContext *ctx, JSValue object, const char *name,
                       int length, int magic, JSValueConst id) {
    JS_DefinePropertyValueStr(ctx,object,name,
        JS_NewCFunctionData(ctx,js_player_method,length,magic,1,&id),
        JS_PROP_ENUMERABLE);
}

static JSValue js_player_open(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    (void)this_val;
    static const char *const OP="audio.player.open";
    if(argc<1||!JS_IsObject(argv[0]))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
            "open(spec, options) needs a spec object",false,
            POCKET_OUTCOME_NOT_APPLIED);
    if(!sound_available())
        return pocket_api_reject(ctx,POCKET_ERR_NOT_AVAILABLE,OP,
            "no audio codec on this unit",false,POCKET_OUTCOME_NOT_APPLIED);
    // Section 9.1: one player, and it is exclusive with the tone. The tone is
    // not stopped for it -- a running tone means this call is early, not that
    // the tone was a mistake.
    if(player.open)
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
            "a player is already open",true,POCKET_OUTCOME_NOT_APPLIED);
    if(tone.active)
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
            "a tone is playing",true,POCKET_OUTCOME_NOT_APPLIED);

    JSValue value=JS_GetPropertyStr(ctx,argv[0],"source");
    if(JS_IsException(value)) return JS_EXCEPTION;
    const char *source=JS_IsString(value)?JS_ToCString(ctx,value):NULL;
    JS_FreeValue(ctx,value);
    if(!source)
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
            "source must be a path such as app:/tune.wav",false,
            POCKET_OUTCOME_NOT_APPLIED);

    av_options_t options;
    JSValue bad=take_options(ctx,argc>1?argv[1]:JS_UNDEFINED,OP,&options);
    if(!JS_IsUndefined(bad)) { JS_FreeCString(ctx,source); return bad; }
    JS_FreeValue(ctx,options.cancel);   // nothing here waits, so nothing polls
    if(options.cancelled) {
        JS_FreeCString(ctx,source);
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,
            "cancelled before the open",false,POCKET_OUTCOME_NOT_APPLIED);
    }

    // Ask the size first so a file that could never fit is refused before an
    // 8 KiB allocation is attempted for it.
    const char *code=NULL;
    int32_t size=pocket_fs_read_all(source,NULL,0,&code);
    if(size<0) {
        JSValue error=pocket_api_reject(ctx,code?code:POCKET_ERR_IO_ERROR,OP,
                                        "the source could not be read",
                                        false,POCKET_OUTCOME_NOT_APPLIED);
        JS_FreeCString(ctx,source);
        return error;
    }
    if(size>PLAYER_MAX_BYTES) {
        JS_FreeCString(ctx,source);
        return pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
            "the file is longer than maxSourceBytes",false,
            POCKET_OUTCOME_NOT_APPLIED);
    }
    uint8_t *file=malloc((size_t)size);
    if(!file) {
        JS_FreeCString(ctx,source);
        return pocket_api_reject(ctx,POCKET_ERR_OUT_OF_MEMORY,OP,
            "no room for the clip",true,POCKET_OUTCOME_NOT_APPLIED);
    }
    int32_t got=pocket_fs_read_all(source,file,(uint32_t)size,&code);
    JS_FreeCString(ctx,source);
    if(got!=size) {
        free(file);
        return pocket_api_reject(ctx,code?code:POCKET_ERR_IO_ERROR,OP,
            "the source could not be read",false,POCKET_OUTCOME_NOT_APPLIED);
    }

    player.file=file;
    const char *why=wav_parse(file,(uint32_t)got);
    if(why) {
        free(file); player.file=NULL;
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,why,false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    player.open=true; player.id=player_next_id++;
    player.position=0; player_reported=0; player.clip=0; player.state=P_READY; player.announce=false;

    JSValue object=JS_NewObject(ctx);
    if(JS_IsException(object)) { player_teardown(); return object; }
    JSValue key=JS_NewInt32(ctx,player.id);
    player_add(ctx,object,"info",0,M_INFO,key);
    player_add(ctx,object,"play",0,M_PLAY,key);
    player_add(ctx,object,"pause",0,M_PAUSE,key);
    player_add(ctx,object,"seek",2,M_SEEK,key);
    player_add(ctx,object,"status",0,M_STATUS,key);
    player_add(ctx,object,"onState",1,M_ON_STATE,key);
    player_add(ctx,object,"close",0,M_CLOSE,key);
    JS_FreeValue(ctx,key);
    // Section 9.1: open takes the header check and the resources, and does not
    // start the sound.
    return pocket_api_settled(ctx,object,false);
}

// ------------------------------------------------------------------- power

// pocket_api.c owns the callback, the handle and the close(); what is left
// beside each subscription is whether it has been told anything yet.
static pocket_sub_slot_t power_slots[POWER_WATCHES];
static bool power_fresh[POWER_WATCHES];   // no delivery yet, so the first poll
                                          // reports whatever it finds
static pocket_sub_table_t power_table = {
    .slots=power_slots, .count=POWER_WATCHES,
    .tag="pocket.av", .what="power",
    // Same rule pocket_imu.c uses: a listener that throws loses its
    // subscription rather than the log and a call every second.
    .close_on_throw=true,
};

static int64_t power_next_us;
static bool    power_primed;   // a sample has been taken since install
static bool    power_have;     // that sample was readable
static int     power_mv;

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
    int slot=0;
    JSValue subscription=pocket_api_sub_open(ctx,&power_table,argv[0],
                                             "power.onChange",
                                             "too many subscriptions",&slot);
    if(JS_IsException(subscription)) return subscription;
    power_fresh[slot]=true;
    power_next_us=0;    // sample on the next frame rather than a second later
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
    *payload=power_state(ctx);
    return true;
}

static void power_pump(void) {
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
    pocket_api_sub_deliver(&power_table,power_payload,&changed);
}

// ------------------------------------------------------------- pump / reset

void pocket_av_pump(void) {
    if(power_table.open) power_pump();
    // Cheaper than it looks when nothing is open: one atomic load and a branch.
    if(player.open||player.announce) player_pump();
}

// Whether either of this file's two namespaces was read. One flag for both:
// the halves below are no-ops for the namespace that was not built -- a table
// with no open slot, a tone nothing could have started -- and two flags would
// be two things to keep in step for no gain.
static bool built;

void pocket_av_reset(void) {
    if(!built) return;
    built=false;
    // The tone is not here: it waits on a promise slot, and pocket_api_reset()
    // is what asks it to stop and lets its resolvers go.
    // The clip is read by the audio task, so this has to be the thing that
    // stops it: the buffer is the host's, but the session ending is what makes
    // it unreachable. Nothing else frees it.
    player_teardown();
    pocket_api_sub_close_all(&player_table);
    player_table.ctx=NULL;
    pocket_api_sub_close_all(&power_table);
    power_table.ctx=NULL;
    power_primed=false;
    power_next_us=0;
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
// Section 2: publish what the code enforces. Every number here is checked in
// this file or in sound.c, and the two that look like omissions are not --
// there is no gain on a clip and no mixing, so neither has a limit to state.
static const pocket_limit_t player_limits[] = {
    {.name="codecs",         .kind=POCKET_LIMIT_TEXT,
     .text=PLAYER_CODEC_PCM "," PLAYER_CODEC_IMA},
    {.name="volumes",        .kind=POCKET_LIMIT_TEXT,.text="app,assets"},
    {.name="sampleRate",     .kind=POCKET_LIMIT_INT,.number=(int32_t)SOUND_SAMPLE_RATE},
    {.name="channels",       .kind=POCKET_LIMIT_INT,.number=1},
    {.name="maxSourceBytes", .kind=POCKET_LIMIT_INT,.number=PLAYER_MAX_BYTES},
    {.name="concurrent",     .kind=POCKET_LIMIT_INT,.number=1},
    {.name="maxWatches",     .kind=POCKET_LIMIT_INT,.number=PLAYER_WATCHES},
    {.name="seekable",       .kind=POCKET_LIMIT_FLAG,.number=1},
    // An ADPCM clip resumes where a block begins, so a seek lands on the block
    // containing the request. 256-byte blocks at 24 kHz are 21 ms; the number
    // is the file's, so this states the shape rather than a value.
    {.name="seekBlockAligned",.kind=POCKET_LIMIT_FLAG,.number=1},
    {.name="streaming",      .kind=POCKET_LIMIT_FLAG,.number=0},
    {.name="mixesWithTone",  .kind=POCKET_LIMIT_FLAG,.number=0},
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
// audio.capture is registered by pocket_capture.c, which owns the microphone
// and contributes `capture` to this namespace.
static const pocket_capability_t audio_cue_capability = {
    .name="audio.cue", .supported=true, .available=false,
    .reason=POCKET_REASON_NO_DEVICE, .limits=cue_limits, .probe=audio_probe,
};
static const pocket_capability_t audio_tone_capability = {
    .name="audio.tone", .supported=true, .available=false,
    .reason=POCKET_REASON_NO_DEVICE, .limits=tone_limits, .probe=audio_probe,
};
// supported=true is the honest answer even though section 9.1's codecs are not
// here: the namespace, the Player and every method of it are implemented, and
// what the build refuses is a file it cannot decode -- an INVALID_ARGUMENT
// about that file, not an UNSUPPORTED about the feature. The codecs limit is
// where an app learns what it may bring.
static const pocket_capability_t audio_playback_capability = {
    .name="audio.playback", .supported=true, .available=false,
    .reason=POCKET_REASON_NO_DEVICE, .limits=player_limits, .probe=audio_probe,
};
static const pocket_capability_t power_capability = {
    .name="power", .supported=true, .available=false,
    .reason=POCKET_REASON_NO_DEVICE, .limits=power_limits, .probe=power_probe,
};

static esp_err_t build_audio(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    // Nothing could have started a tone without this namespace, so this is the
    // one place it needs clearing.
    tone.active=false;
    JS_DefinePropertyValueStr(ctx,ns,"cue",
        JS_NewCFunction(ctx,js_cue,"cue",1),JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx,ns,"tone",
        JS_NewCFunction(ctx,js_tone,"tone",2),JS_PROP_ENUMERABLE);
    // audio.capture is pocket_capture.c's, contributed to this same
    // namespace by a second lazy builder.
    // A realm going away takes its listeners with it, so the table starts empty
    // every time the namespace is built.
    for(int i=0;i<PLAYER_WATCHES;i++) {
        player_slots[i].callback=JS_UNDEFINED;
        player_slots[i].handle=0;
    }
    player_table.open=0;
    player_table.ctx=ctx;
    JSValue object=JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx,object,"open",
        JS_NewCFunction(ctx,js_player_open,"open",2),JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx,ns,"player",object,JS_PROP_ENUMERABLE);
    built=true;
    return ESP_OK;
}

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
    power_primed=false;
    JS_DefinePropertyValueStr(ctx,ns,"status",
        JS_NewCFunction(ctx,js_status,"status",0),JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx,ns,"onChange",
        JS_NewCFunction(ctx,js_on_change,"onChange",1),JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx,ns,"keepAwake",
        JS_NewCFunction(ctx,js_keep_awake,"keepAwake",1),JS_PROP_ENUMERABLE);
    built=true;
    return ESP_OK;
}

esp_err_t pocket_av_install(JSContext *ctx, void *user_data) {
    (void)user_data;
    pocket_api_register(&audio_cue_capability);
    pocket_api_register(&audio_tone_capability);
    pocket_api_register(&audio_playback_capability);
    pocket_api_register(&power_capability);
    esp_err_t err=pocket_api_lazy(ctx,"audio",build_audio,NULL);
    if(err!=ESP_OK) return err;
    return pocket_api_lazy(ctx,"power",build_power,NULL);
}
