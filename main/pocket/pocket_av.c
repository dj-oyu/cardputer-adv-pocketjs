#include "pocket_av.h"
#include "pocket_api.h"
#include "pocket_fs.h"
#include "opus_feed.h"
#include "mp3_feed.h"
#include "mp3_decode.h"
#include "opus_net.h"
#include "sound.h"
#include "board.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The pool every allocation in this file competes for, and the one the two
// numbers in a play() refusal are read from.
#define AV_HEAP_POOL (MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)

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
// One Player and one 24 kHz mono output path: WAV is fed directly, Opus and
// MP3 use playback-only decode workers. MP3 also converts rate and channels.
// File sources use the existing app/assets/granted-sd readers; HTTP is Opus.
//
// IT IS STREAMED. That is the whole of what changed, and it changed because the
// previous shape was diagnosed backwards. A clip was read into RAM whole at
// open and decoded in place, under a PLAYER_MAX_BYTES of 24,576 -- which people
// read as a codec problem and answered by shopping for codecs. It was not: a
// sound longer than the buffer cannot play however well it compresses, so the
// buffer was the ceiling. Now the ring is what is resident (6,144 bytes, and
// only while a player has been asked to play) and the source is what is long.
//
// What that is worth TODAY is smaller than it sounds, and saying so is the
// point of this paragraph. `app:/` caps one file at 24,576 bytes in the
// filesystem itself -- FS_MAX_FILE in pocket_fs.c, derived from the 16-sector
// quota and the requirement that a replace hold two versions at once -- which
// is the same number PLAYER_MAX_BYTES was. So on app: the length does not move
// at all; what moves is that the player stops spending 24,576 bytes of heap to
// hold a sound it is playing 2,048 bytes of. The length only moves when the
// source can be longer: `assets:/`, which is flash-mapped firmware content and
// has no such cap, or sd: when it lands. The player no longer has an opinion
// either way, which is the part that was worth building.
//
// The producer is pocket_av_pump(), on the JS/ui task, because that is the task
// pocket_fs.c's reads belong to -- its store index and block walk are not
// thread-safe and nothing here is going to make them so. A refill is one
// esp_partition_read of at most 2,048 bytes; at PCM16's 48,000 bytes a second
// that is about 23 reads a second, at most one slot-full per frame after the
// first. It is the same work fs.file.read already does on this task on an app's
// behalf, and it is nowhere near the "long operation on the drawing task" that
// froze the panel for sound_capture_probe().
//
// MEASURED ON THE BOARD (2026-09-08), and it settled a question this file used
// to carry only an estimate for. apps/streamplay times 60 idle frames against a
// whole play, on one binary, and repeats. Thirteen consecutive cycles:
//
//   underruns=0   deltaTenthMs -2..-4   worstFrameMs 35..36
//   idleTenthMs 337..338                playTenthMs 334..335
//
// deltaTenthMs is playing-mean minus idle-mean in tenths of a millisecond, so
// feeding measured very slightly NEGATIVE. That is not a saving, it is noise:
// the honest statement is that THE COST OF FEEDING IS BELOW WHAT THIS
// INSTRUMENT CAN RESOLVE, against a predicted bound of 1 ms. Nobody should
// quote -0.3 ms as a speed-up.
//
// The thirteen cycles matter more than the first one. Each cycle is a fresh
// open/close, so a delta that grew across them would have meant something
// leaking through the player's lifetime; identical cycles say there is not one.
// A single measurement could not have told those apart.
//
// The consumer is the audio task, as before. Between them, `underruns` finally
// means something: it counts 128-frame blocks the audio task had to fill with
// silence because the pump had not got back yet.
//
// WHERE THIS DESIGN DOES NOT REACH, AND IT IS sd:. Feeding from the ui task is
// safe because a flash read is short. A card read is not. The figures are
// sd-surface's and they are arithmetic, not measurement -- nobody has timed a
// card-backed stream on this board:
//
//   * the bus runs at 400 kHz, so one 2,048-byte slot is about 41 ms of bus
//     time IN WHATEVER FRAME ASKS FOR IT, before the directory walk and open
//     that pocket_fs_read_at() does per call. A 33 ms frame cannot contain
//     that, and a frame that does not finish is a panel that stops -- the exact
//     failure sound_capture_probe() was moved off this task to avoid.
//   * 24 kHz mono PCM16 is 48,000 B/s = 384 kbit/s against that 400 kbit/s bus,
//     so card-backed PCM16 cannot sustain realtime at this clock at all,
//     whoever feeds it. ADPCM's 12,000 B/s fits the bandwidth with room, but
//     each refill still stalls its frame for as long.
//
// So `assets:/` and `app:/` are what this feeder serves, and a card source will
// stutter and drag the frame rather than fail cleanly. Two ways out, and they
// are different work: raise the SD clock (sd_media.c says that is a measurement
// rather than an edit, and MISO is wired so it can be checked in software), or
// move the feed to a task of its own at priority 4 -- which needs the fs
// surface made thread-safe first, and is why it was not done here. NEITHER IS
// WORTH STARTING ON THE ARITHMETIC ALONE. Time a card read first.

// PLAYER_MAX_BYTES WAS HERE, and it was 24,576. It is gone rather than moved,
// and the reason is worth the paragraph because three people in a row have now
// answered this question with the previous answer.
//
// The blame has moved twice. It was read as a CODEC limit -- 2.05 s of ADPCM,
// so find a better codec -- and the answer to that was: no, THE CAP IS THE
// BUFFER, a sound longer than the buffer cannot play however well it
// compresses. That correction was right and still incomplete. Measure the next
// step and the cap is neither: for `app:/` it is THE FILESYSTEM QUOTA.
// FS_MAX_FILE in pocket_fs.c is 24,576, derived from the 16-sector quota and
// the requirement that a replace hold two versions at once, and PLAYER_MAX_
// BYTES was set to match it.
//
// SO THE TWO NUMBERS WERE EQUAL BY DERIVATION, NOT BY COINCIDENCE. Anybody who
// moves one of them and expects a longer clip on app: will find nothing
// changes, and will not be able to explain why. The buffer is now gone and the
// quota is still there; app: is still 24,576 bytes of source and always was.
//
// What streaming actually changed is therefore two things, neither of them
// length on app:. First, the 24,576-byte allocation a player held from open()
// became a 6,144-byte ring held only while it plays. Second, and this is the
// deliverable: the player no longer has an opinion about how long a source is,
// so a volume that can hold a longer one needs no second rewrite here. sd: is
// that volume and it has landed; assets: is flash-mapped firmware content and
// never had the cap either.
//
// Each step in that chain looked like the answer until somebody measured the
// next one. Assume there is another.

// How much of the file the header walk may look at. The walk itself is ranged
// -- eight bytes to read a chunk header, then a skip -- so a big LIST in front
// of the data costs reads and not RAM; this bounds how many chunks a file may
// make us walk before it is called malformed.
#define PLAYER_MAX_CHUNKS 64
// Pump calls to wait for the decoder's first slot before calling it a failure.
// Priming is measured in a couple of frames (prime_us in the OPUSDEC line says
// how many microseconds it really took); 30 frames is about a second, which is
// far past anything healthy and short enough that a decoder that died without
// saying so becomes an error rather than a player stuck in "playing" in silence.
#define PLAYER_PRIME_FRAMES 30
#define PLAYER_WATCHES   2
#define PLAYER_CODEC_PCM  "wav/pcm16"
#define PLAYER_CODEC_IMA  "wav/ima-adpcm"
// Named for what it decodes rather than for what Opus can carry, because the
// gate in opus_feed.h refuses the rest of Opus by name. An app that feature-tests
// this string and then hands us a SILK asset would otherwise be surprised at
// play() instead of at open().
//
// WIDENING THIS TO "opus" IS NOT A ONE-LINE CHANGE, AND THIS IS THE LINE
// SOMEBODY WILL BE STANDING ON WHEN THEY TRY. The decode task's stack is 12,288
// bytes and the measured high-water behind it is 10,476 -- 17% of margin --
// which is only defensible because the CELT 20 ms mono gate makes that the only
// path that can run. SILK and hybrid are different code with a different peak
// that NOBODY HAS MEASURED on this part. So the order is: measure those peaks
// first, then move DEC_STACK in opus_feed.c, then find the contiguity that the
// bigger stack needs (18,436 + DEC_STACK has to come out of one heap run, and on
// a board with a linked radio that run has been measured at 31,744), and only
// then widen this string.
#define PLAYER_CODEC_OPUS "opus/celt"
#define PLAYER_CODEC_MP3 "mp3"
// Unknown duration until EOF; leave room for sound.c's 256-frame DMA tail.
#define PLAYER_MP3_UNKNOWN (UINT32_MAX-256u)
#define PLAYER_RING_BYTES (SOUND_STREAM_SLOTS*SOUND_STREAM_SLOT_BYTES)
// The compressed ring, and it is the same struct and the same slot geometry as
// the PCM one on purpose: those atomics are the part of streaming whose mistakes
// are silent, and tools/test_stream.c already compiles and exercises them. A
// second, smaller, hand-written ring would have saved 4 KiB of playtime heap and
// duplicated exactly the code that header exists to keep un-duplicated.
//
// What 6,144 bytes buys on this side is not 128 ms but 2.0 seconds: Opus at
// 24 kbps is 3,050 bytes a second, sixteen times denser than the PCM16 the same
// ring holds downstream. That is the slack the ui task's 39.9 ms frames are paid
// out of, and it is why nothing here has to become thread-safe.
#define PLAYER_PKT_BYTES  (SOUND_STREAM_SLOTS*SOUND_STREAM_SLOT_BYTES)

typedef enum { P_READY=0, P_PLAYING, P_PAUSED, P_ENDED, P_ERROR } player_state_t;
// Which of the three the open source turned out to be. `block` used to carry
// this on its own (nonzero meant ADPCM); a third codec needs a name.
typedef enum { C_PCM16=0, C_IMA, C_OPUS, C_MP3 } player_codec_t;
static const char *const PLAYER_STATE_NAME[]={
    "ready","playing","paused","ended","error" };

static struct {
    bool     open;
    player_codec_t codec;
    int32_t  id;            // identity the bound methods carry; never reused
    char    *path;          // the source, kept because every refill re-reads it
    uint32_t offset, bytes; // the data chunk within the file
    uint16_t block;         // ADPCM block size, 0 for PCM16
    uint32_t per_block;     // output frames one ADPCM block is worth
    uint32_t frames;        // the clip's length in output frames
    // MP3 only, and SEPARATE from `frames` on purpose. `frames` drives when
    // playback ends, and for MP3 that is EOF rather than a count -- setting it
    // from a header would make the end of the song a prediction, and a file
    // whose tag disagreed with its contents would stop early or hang. This is
    // for DISPLAY: it is what info() reports, and nothing reads it back.
    // 0 means the file did not say.
    uint32_t mp3_duration_ms;
    uint32_t position;      // frames consumed before the running stream started
    uint32_t feed;          // the next byte of the file the pump will read
    uint32_t underruns;     // carried across pause and seek; see M_STATUS
    uint8_t *ring_bytes;    // PLAYER_RING_BYTES, or NULL before the first play
    sound_stream_t ring;
    // Opus only, and NULL for everything else: the compressed ring between the
    // pump and the decode task, and the container the header described.
    uint8_t *pkt_bytes;
    sound_stream_t pkt;
    opus_pak_t pak;
    // An http(s) source: the packet ring is filled by opus_net.c's receive task
    // instead of by player_feed_opus() on this one, and the container header
    // arrives over the socket rather than from a read at offset 0. Everything
    // downstream of the ring -- the decode task, the PCM ring, sound.c -- cannot
    // tell the difference, which is the whole point.
    bool     net;
    pocket_request_t open_req;  // the open() this stream has still to settle
    int32_t  stream;        // sound.c's id, 0 when nothing is queued
    // Opus only: the audio task has not been started yet because the decode task
    // has not produced anything yet. See the priming block in player_pump().
    bool     priming;
    uint16_t prime_waits;
    uint32_t mp3_progress;
    bool source_fault;
    player_state_t state;
    bool     announce;      // a state change the pump has still to deliver
} player;

static int32_t player_next_id=1;

// The audio task's hand-back. Same rule as the tone's: post and stop.
//
// What the callback carries is a launch number, not sound.c's stream id: a
// source short enough can be over before sound_stream_start() has returned that
// id, and a completion nobody can name yet is a completion that gets applied to
// the wrong thing. The number is minted before the launch and moved on by every
// halt, so a stopped stream's completion matches nothing and is dropped -- the
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
// The walk is ranged now rather than over a buffer, which is what lets a file
// be longer than anything this host will hold: eight bytes to read a chunk
// header, sixteen or twenty more for a fmt body, and a skip for everything
// else. Every length is checked against the file size before it is used, so a
// truncated or lying header ends as a refusal rather than as a read past the
// end. PLAYER_MAX_CHUNKS bounds the walk itself.
static const char *player_at(uint32_t at, uint8_t *out, uint32_t want) {
    const char *code=NULL;
    int32_t got=pocket_fs_read_at(player.path,at,out,want,&code);
    if(got!=(int32_t)want) return "the source could not be read";
    return NULL;
}

static const char *wav_parse(uint32_t size) {
    uint8_t head[24];
    if(size<44) return "not a WAV file";
    const char *why=player_at(0,head,12);
    if(why) return why;
    if(memcmp(head,"RIFF",4)||memcmp(head+8,"WAVE",4)) return "not a WAV file";
    uint32_t at=12, rate=0, per_block=0;
    uint16_t format=0, channels=0, bits=0, align=0;
    bool have_fmt=false;
    player.offset=player.bytes=0;
    for(unsigned n=0;n<PLAYER_MAX_CHUNKS&&at+8<=size;n++) {
        if((why=player_at(at,head,8))) return why;
        uint32_t body=(uint32_t)head[4]|((uint32_t)head[5]<<8)|
                      ((uint32_t)head[6]<<16)|((uint32_t)head[7]<<24);
        if(body>size-at-8) return "a chunk runs past the end of the file";
        if(!memcmp(head,"fmt ",4)&&body>=16) {
            uint8_t f[20];
            uint32_t want=body>=20?20:16;
            if((why=player_at(at+8,f,want))) return why;
            format=(uint16_t)(f[0]|(f[1]<<8));
            channels=(uint16_t)(f[2]|(f[3]<<8));
            rate=(uint32_t)f[4]|((uint32_t)f[5]<<8)|
                 ((uint32_t)f[6]<<16)|((uint32_t)f[7]<<24);
            align=(uint16_t)(f[12]|(f[13]<<8));
            bits=(uint16_t)(f[14]|(f[15]<<8));
            // IMA carries its samples-per-block in the fmt extension. Trusting
            // it rather than deriving it is what lets a file made by a tool
            // that pads its blocks still decode where its blocks really begin.
            if(want==20) per_block=(uint32_t)(f[18]|(f[19]<<8));
            have_fmt=true;
        } else if(!memcmp(head,"data",4)) {
            player.offset=at+8; player.bytes=body;
        }
        at+=8+body+(body&1);    // chunks are padded to an even length
    }
    if(!have_fmt) return "the file has no fmt chunk";
    if(!player.bytes) return "the file has no audio in it";
    if(channels!=1) return "this host plays one channel";
    if(rate!=SOUND_SAMPLE_RATE) return "this host plays 24000 Hz and has no resampler";
    if(format==1&&bits==16) {
        player.block=0; player.per_block=1;
        player.frames=player.bytes/2;
    } else if(format==0x11&&bits==4) {
        player.codec=C_IMA;
        if(align<8||(align&1)||align>player.bytes) return "the ADPCM block size is not usable";
        // A block has to fit one slot whole or a slot boundary would land
        // mid-block, where there is nothing to reseed the predictor from. This
        // is the one thing streaming refuses that the RAM clip did not, and it
        // is 2,048 bytes: 4,093 output frames, 170 ms of audio in one block.
        if(align>SOUND_STREAM_SLOT_BYTES) return "the ADPCM block is larger than the stream slot";
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

// The Opus container, and the two things open() has to establish about it: that
// the header describes something this host decodes, and that the FIRST PACKET
// really is CELT 20 ms mono. The second is one four-byte read and it is what
// turns a wrongly-encoded asset into a refusal at open() rather than into a
// stream that dies three slots in. Every later packet is gated in the decode
// task, where a refusal ends the stream as an error.
static const char *opus_parse(uint32_t size) {
    uint8_t head[OPUS_PAK_HEADER];
    const char *why=player_at(0,head,OPUS_PAK_HEADER);
    if(why) return why;
    if((why=opus_pak_parse(head,size,&player.pak))) return why;
    uint8_t first[3];
    if((why=player_at(player.pak.data_offset,first,3))) return why;
    if(((uint32_t)first[0]|((uint32_t)first[1]<<8))<1)
        return "the first Opus packet is empty";
    if(!opus_pak_toc_ok(first[2]))
        return "this host decodes CELT 20 ms mono Opus only";
    player.codec=C_OPUS;
    player.block=0; player.per_block=1;
    player.offset=player.pak.data_offset;
    player.bytes=size-player.pak.data_offset;
    player.frames=player.pak.total_frames;
    return NULL;
}

// Which of the three the file is. The magic decides, not the extension: a name
// is what a user typed and a magic is what a tool wrote.
static const char *source_parse(uint32_t size) {
    uint8_t magic[4];
    const char *why=player_at(0,magic,4);
    if(why) return why;
    if(!memcmp(magic,"POK1",4)) return opus_parse(size);
    if(!memcmp(magic,"ID3",3)||(magic[0]==255&&(magic[1]&224)==224)) {
        uint32_t offset=0, end=size;
        if(!memcmp(magic,"ID3",3)) {
            uint8_t tag[10];
            if(size<10||(why=player_at(0,tag,10))) return "truncated ID3 tag";
            if(tag[3]<2||tag[3]>4||tag[4]==255||
               (tag[6]|tag[7]|tag[8]|tag[9])&128) return "unsupported ID3 tag";
            offset=10u+((uint32_t)tag[6]<<21)+((uint32_t)tag[7]<<14)+
                   ((uint32_t)tag[8]<<7)+tag[9];
            if(tag[3]==4&&(tag[5]&16)) offset+=10;
        }
        if(size>=128) {
            uint8_t tail[3];
            if((why=player_at(size-128,tail,3))) return why;
            if(!memcmp(tail,"TAG",3)) end-=128;
        }
        pocket_mp3_header_t h;
        if(offset>=end||end-offset<4) return "the MP3 has no audio";
        if((why=player_at(offset,magic,4))) return why;
        if(!pocket_mp3_header(magic,&h)||h.bytes>end-offset)
            return "expected MPEG Layer III (free-format is unsupported)";
        player.codec=C_MP3; player.block=0; player.per_block=1;
        player.offset=offset; player.bytes=end-offset;
        player.frames=PLAYER_MP3_UNKNOWN;
        // The length, if the encoder wrote one. 64 bytes covers both tags; a
        // frame shorter than that is not one that carries either.
        player.mp3_duration_ms=0;
        uint8_t head[64];
        unsigned want=h.bytes<sizeof head?h.bytes:sizeof head;
        if(want>=48&&!player_at(offset,head,want)) {
            uint32_t total=pocket_mp3_total_frames(head,want,&h);
            // In the FILE's sample rate, not the output's: the rate converter
            // downstream changes how many samples come out and not how long the
            // song is. Rounded rather than truncated because a bar that stops
            // one millisecond short of the end looks like a bug.
            if(total&&h.rate)
                player.mp3_duration_ms=(uint32_t)(((uint64_t)total*h.samples*1000u
                                                   +h.rate/2)/h.rate);
        }
        return NULL;
    }
    player.codec=C_PCM16;              // wav_parse promotes this to C_IMA
    return wav_parse(size);
}

// ---- state

static void player_set_state(player_state_t state) {
    if(player.state==state) return;
    player.state=state;
    player.announce=true;   // delivered from the pump, never inside a JS call
}

// sound_stream_position() answers 0 both before the audio task has reached the
// stream and after it is over, and the pump is what tells those apart -- so
// between the last sample and the next frame the raw sum would walk backwards
// to where play() started. Keeping the highest reading is enough: within one
// stream the position only ever grows, and a halt or a seek sets it afresh.
static uint32_t player_reported;

static uint32_t player_frames_now(void) {
    if(player.state!=P_PLAYING||!player.stream) return player.position;
    uint32_t done=player.position+sound_stream_position(player.stream);
    if(done>player.frames) done=player.frames;
    if(done<player_reported) return player_reported;
    player_reported=done;
    return done;
}

// Fills every free slot from the source. The producer half of sound.h's ring,
// and the only thing on the JS task that touches the file after open.
//
// It publishes the last slot with `last` set rather than a zero-length one
// afterwards, so the audio task learns the source is finished at the same
// moment it gets the bytes that finish it, and never spends a slot cycle on an
// empty slot to be told.
// The Opus half of the refill. Same job as the one below -- fill every free slot
// from the file -- but the ring it fills carries compressed packets for the
// decode task rather than samples for the audio task, and a slot has to end
// where a packet does for the same reason an ADPCM slot has to end where a block
// does. opus_pak_whole() is that trim; the bytes it leaves behind are read again
// as the head of the next slot, which costs one short re-read per slot and saves
// carrying a partial packet across the boundary.
static void player_feed_opus(void) {
    uint32_t end=player.offset+player.bytes;
    for(;;) {
        if(player.feed>=end) return;
        uint8_t *slot=sound_stream_slot(&player.pkt);
        if(!slot) return;
        uint32_t want=SOUND_STREAM_SLOT_BYTES, left=end-player.feed;
        if(want>left) want=left;
        const char *code=NULL;
        int32_t got=pocket_fs_read_at(player.path,player.feed,slot,want,&code);
        if(got<=0) {
            ESP_LOGW("pocket.av","the source stopped reading at %u",
                     (unsigned)player.feed);
            sound_stream_publish(&player.pkt,0,true);
            player.feed=end;
            return;
        }
        uint32_t whole=opus_pak_whole(slot,(uint32_t)got);
        // Zero means the first packet in this slot claims to be longer than a
        // slot, which opus_pak_parse already refused through maxPacketBytes -- so
        // reaching here means the file disagrees with its own header. Ending is
        // the honest move; retrying would read the same bytes forever.
        bool last=player.feed+(uint32_t)got>=end;
        if(!whole) {
            ESP_LOGE("pocket.av","a packet at %u does not fit a slot",
                     (unsigned)player.feed);
            sound_stream_publish(&player.pkt,0,true);
            player.feed=end;
            return;
        }
        player.feed+=whole;
        // A tail shorter than its own length prefix is a truncated file. The
        // whole packets before it still play; the stream ends where they do.
        if(last) player.feed=end;
        sound_stream_publish(&player.pkt,whole,last);
    }
}

static void player_feed(void) {
    // A network source's producer is opus_net.c's task, not this one. There is
    // nothing for the pump to do and, more to the point, nothing it MAY do: the
    // ring has a single producer and that is what makes its atomics correct.
    if(player.net) return;
    if(player.codec==C_OPUS) { if(player.pkt_bytes) player_feed_opus(); return; }
    if(!player.ring_bytes) return;
    sound_stream_t *destination=player.codec==C_MP3?&player.pkt:&player.ring;
    if(player.codec==C_MP3&&!player.pkt_bytes) return;
    uint32_t end=player.offset+player.bytes;
    for(;;) {
        if(player.feed>=end) return;            // the last slot carried `last`
        uint8_t *slot=sound_stream_slot(destination);
        if(!slot) return;
        uint32_t want=SOUND_STREAM_SLOT_BYTES;
        // A slot holds whole ADPCM blocks; see sound.h for why that is what
        // makes a slot boundary cost the decoder nothing.
        if(player.block) want-=want%player.block;
        uint32_t left=end-player.feed;
        if(want>left) want=left;
        const char *code=NULL;
        int32_t got=pocket_fs_read_at(player.path,player.feed,slot,want,&code);
        if(got<=0) {
            // The source stopped answering mid-stream. Ending it here is the
            // honest move: the audio task plays what it already has and the
            // completion reports the shortfall, which reaches the app as the
            // same "stopped early" P_ERROR an I2S failure does.
            ESP_LOGW("pocket.av","the source stopped reading at %u",
                     (unsigned)player.feed);
            player.source_fault=true;
            sound_stream_publish(destination,0,true);
            player.feed=end;
            return;
        }
        player.feed+=(uint32_t)got;
        if(player.codec==C_MP3) {
            sound_stream_publish(destination,(uint32_t)got,false);
            if(player.feed>=end) atomic_store(&destination->eof,true);
        } else sound_stream_publish(destination,(uint32_t)got,player.feed>=end);
    }
}

// Stops whatever is sounding and takes the position and the underruns with it.
// The reads have to come first: sound_stream_position() answers 0 once the
// stream is over, and the stop is what makes it over. The few frames the audio
// task may add between the two are lost, which is a position up to 5ms behind
// and never ahead.
static bool player_halt(void) {
    // No sound stream is not the same as nothing running. The decode task can
    // outlive the audio task -- the audio task stops when it has played the
    // frames it was promised, and the decoder is at that moment parked waiting
    // for a slot that will never come free. Returning true here without asking
    // it to stop would let teardown free two rings it is still holding.
    if(!player.stream) {
        if(player.codec!=C_OPUS&&player.codec!=C_MP3) return true;
        // Priming counts as running: the decode task is alive and holding both
        // rings even though nothing is sounding yet.
        player.priming=false;
        bool freed=player.codec==C_MP3?mp3_feed_stop():opus_feed_stop();
        if(player.net) freed=opus_net_stop()&&freed;
        return freed;
    }
    player.position=player_frames_now();
    player.underruns+=sound_stream_underruns();
    bool released=sound_stream_stop(player.stream);
    // The audio task first, then the decode task: stopping the consumer first
    // means the decoder finds the PCM ring full and parks rather than spinning
    // through the packets it had left. Both have to say they are out before
    // either ring can be freed, so the two answers are ANDed rather than the
    // second one overwriting the first.
    if(player.codec==C_OPUS) released=opus_feed_stop()&&released;
    if(player.codec==C_MP3) released=mp3_feed_stop()&&released;
    // After the decoder, because the decoder is what reads the packet ring: a
    // receiver stopped first would leave it blocked on a ring nobody fills.
    if(player.net) released=opus_net_stop()&&released;
    player.stream=0;
    player_seq++;   // the completion this stop provokes now matches nothing
    return released;
}

// Queues the rest of the source from `player.position`. ADPCM is only resumable
// where a block begins -- a block reseeds the decoder, mid-block there is
// nothing to reseed from -- so the position moves back to the start of the
// block it lands in. That rounding is the whole of seekResolutionMs.
// Where in the file an Opus stream restarts from, and what the position becomes.
//
// Opus packets are variable length, so a byte offset is not derivable from a
// frame number -- which is what the container's index is for. One entry per
// indexInterval packets, so a seek lands on the entry at or before the request
// and the position moves back to that packet's first frame. Same rounding an
// ADPCM block already imposes, at 100 ms rather than 21.
//
// The reads: four bytes. This runs on the drawing task and it is not allowed to
// walk the file.
//
// `skip` is the encoder lookahead, and it is only the first entry that carries
// it -- header totalFrames already excludes those samples, so entry 0 has to
// drop them to make the two agree, and any later entry is thousands of samples
// past them.
static const char *player_locate_opus(uint32_t *start, uint32_t *byte,
                                      uint16_t *skip) {
    const opus_pak_t *k=&player.pak;
    uint32_t packet=(*start+k->pre_skip)/k->frame_samples;
    uint32_t entry=packet/k->index_interval;
    if(entry>=k->index_count) entry=k->index_count-1;
    uint8_t at[4];
    const char *why=player_at(k->index_offset+entry*4,at,4);
    if(why) return why;
    uint32_t offset=(uint32_t)at[0]|((uint32_t)at[1]<<8)|
                    ((uint32_t)at[2]<<16)|((uint32_t)at[3]<<24);
    if(offset<k->data_offset||offset>=k->file_bytes)
        return "the seek index points outside the file";
    uint32_t first=entry*(uint32_t)k->index_interval*k->frame_samples;
    *skip=entry?0:k->pre_skip;
    *start=entry?first-k->pre_skip:0;
    *byte=offset;
    return NULL;
}

static const char *player_launch(void) {
    uint32_t start=player.position, byte=player.offset;
    uint16_t skip=0;
    // A network source cannot be located: there is no index to consult and no
    // way back to a byte already received. It plays from where the socket is,
    // which is the head, and preSkip is the header's -- the same value a file
    // uses on its first packet.
    if(player.net) {
        start=0;
        skip=player.pak.pre_skip;
    } else if(player.codec==C_OPUS) {
        const char *why=player_locate_opus(&start,&byte,&skip);
        if(why) return "unavailable";
    } else if(player.block) {
        uint32_t index=start/player.per_block;
        start=index*player.per_block;
        byte+=index*player.block;
    } else if(player.codec!=C_MP3) byte+=start*2;
    // The byte half of this has no meaning for a network source: there is no
    // data chunk, only a socket that is already delivering.
    if(start>=player.frames||(!player.net&&byte>=player.offset+player.bytes))
        return "ended";
    // The ring is taken at the first play and held until close, not taken and
    // given back around every pause: 6,144 bytes churned on each pause would
    // fragment a heap whose largest block is the thing this whole surface has
    // to fit inside. A player that is opened and never played still costs
    // nothing, which is the case that matters.
    if(!player.ring_bytes) {
        player.ring_bytes=malloc(PLAYER_RING_BYTES);
        if(!player.ring_bytes) return "nomem";
        player.ring.bytes=player.ring_bytes;
    }
    // The compressed ring, and it exists only for an Opus source. Held to close
    // for the same reason the PCM one is: churning 6 KiB on every pause is how a
    // heap whose largest block this whole surface has to fit inside gets
    // fragmented.
    if((player.codec==C_OPUS||player.codec==C_MP3)&&!player.pkt_bytes) {
        player.pkt_bytes=malloc(PLAYER_PKT_BYTES);
        if(!player.pkt_bytes) return "nomem";
        player.pkt.bytes=player.pkt_bytes;
    }
    player.position=start;
    player_reported=start;
    player.feed=byte;
    player.source_fault=false;
    sound_stream_rewind(&player.ring);
    uint32_t seq=++player_seq;
    // Primed before the audio task is given anything to play, so the first
    // block is audio rather than an underrun. Three slots is one read of at
    // most 6,144 bytes in total.
    if(player.codec==C_MP3) {
        sound_stream_rewind(&player.pkt);
        player_feed();
        mp3_feed_start_t started=mp3_feed_start(&player.ring,&player.pkt,start);
        if(started!=MP3_FEED_OK) {
            player_seq++;
            return started==MP3_FEED_NOMEM?"nomem":"busy";
        }
        player.priming=true; player.prime_waits=0; player.mp3_progress=0;
        return NULL;
    }
    if(player.codec==C_OPUS) {
        // The packet ring is NOT rewound for a network source: its producer has
        // been filling it since open() and rewinding under a running task is
        // exactly the race those atomics are shaped to avoid.
        if(!player.net) sound_stream_rewind(&player.pkt);
        player_feed();          // compressed slots, so the decoder has work
        // The decode task is created here and deleted when the stream ends, so
        // Opus costs nothing at rest -- 18,436 bytes of decoder state and 14,336
        // of stack are taken now and given back at stop.
        opus_feed_start_t started=opus_feed_start(&player.ring,&player.pkt,
                                                  player.frames-start,skip);
        if(started!=OPUS_FEED_OK) {
            player_seq++;
            // NOMEM is answered as OUT_OF_MEMORY with numbers rather than as a
            // flat NOT_AVAILABLE, because on this board running out of room is
            // the expected failure and "unavailable" is the one word that tells
            // an app nothing it can act on.
            return started==OPUS_FEED_NOMEM?"nomem":"busy";
        }
        // AND THE AUDIO TASK IS NOT STARTED HERE. This line used to be shared
        // with the WAV path below, under a comment claiming that starting the
        // audio task after the decode task meant the ring had samples in it
        // first. That ordered the two CALLS, not the two pieces of work:
        // opus_feed_start() creates a task and returns before it has run, so the
        // audio task was handed an EMPTY ring and every 128-frame block it could
        // not fill was an underrun. Measured on the board: 5 of them, 26.7 ms,
        // which is what a cold opus_decoder_create plus two cold decodes costs.
        //
        // The WAV path has no such gap because ITS producer is player_feed(), a
        // function call on this task -- it has genuinely finished when the line
        // after it runs. A task cannot be primed by calling a function, so the
        // start moves to the pump, which begins the sound on the first frame
        // that finds a slot published. Section 9.1 has play() resolve on the
        // output being accepted rather than on sound being heard, so spending
        // one frame here costs the contract nothing and blocks nobody: the
        // alternative was blocking the drawing task for the length of a decode.
        player.priming=true;
        player.prime_waits=0;
        return NULL;
    }
    player_feed();
    int32_t id=sound_stream_start(&player.ring,
                                  player.block?SOUND_STREAM_IMA:SOUND_STREAM_PCM16,
                                  player.block,player.frames-start,1.0f,
                                  clip_done,(void *)(uintptr_t)seq);
    if(id<0) { player_seq++; return id==SOUND_ERR_BUSY?"busy":"unavailable"; }
    player.stream=id;
    return NULL;
}

static void player_teardown(void) {
    if(!player.open) return;
    player.open=false;
    if(player_halt()) { free(player.ring_bytes); free(player.pkt_bytes); }
    else ESP_LOGE("pocket.av","a task still holds the rings; their %u bytes stay",
                  (unsigned)(PLAYER_RING_BYTES+
                             ((player.codec==C_OPUS||player.codec==C_MP3)?PLAYER_PKT_BYTES:0)));
    player.ring_bytes=NULL; player.ring.bytes=NULL;
    player.pkt_bytes=NULL; player.pkt.bytes=NULL;
    free(player.path); player.path=NULL;
    player.state=P_READY; player.announce=false;
    player.position=0; player.frames=0; player.bytes=0;
    player.mp3_duration_ms=0;
    player.feed=0; player.underruns=0;
    player.priming=false; player.prime_waits=0;
    player.codec=C_PCM16;
    // Back to the file shape. Left true, this would send the NEXT player's pump
    // and feed down the network branches for a source that has no receiver.
    player.net=false; player.open_req=0;
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
    // What can now fail asynchronously: an I2S write that stopped early, and a
    // producer that could not keep the ring fed for 2.7 seconds. Both are
    // P_ERROR and both are honestly "the stream stopped early"; which of the
    // two it was is in the log, not in the app's error, because an app can do
    // nothing different about either.
    JS_SetPropertyStr(ctx,event,"error",
        player.state==P_ERROR
            ?pocket_api_error(ctx,POCKET_ERR_IO_ERROR,"audio.player",
                              "the stream stopped early",true,
                              POCKET_OUTCOME_UNKNOWN)
            :JS_NULL);
    *payload=event;
    return true;
}

static void player_pump(void) {
    // The network open, settled here because opus_net.c runs on its own task and
    // pocket_api_complete() is the only thing it is allowed to touch. Errors are
    // checked FIRST: a receiver that failed after publishing its header would
    // otherwise be reported as a successful open of a stream that is already
    // dead.
    if(player.open_req) {
        if(opus_net_error()) {
            pocket_api_complete(player.open_req,1);
        } else if(opus_net_ready()) {
            player.pak=*opus_net_header();
            player.offset=player.pak.data_offset;
            player.bytes=0;                  // no data chunk: there is a socket
            player.frames=player.pak.total_frames;
            pocket_api_complete(player.open_req,POCKET_STATUS_OK);
        }
    }
    // The Opus start, deferred out of play() -- see player_launch(). One slot is
    // 40 ms of decoded audio, which is a whole frame of head start for a
    // consumer that takes 5.3 ms at a time; eof covers a source so short the
    // decoder finished it before publishing twice.
    if(player.priming) {
        player_feed();          // keep the packet ring full while we wait
        if(player.codec==C_MP3&&player.mp3_progress!=mp3_feed_progress()) {
            player.mp3_progress=mp3_feed_progress(); player.prime_waits=0;
        }
        if(atomic_load(&player.ring.filled)||atomic_load(&player.ring.eof)) {
            player.priming=false;
            int32_t id=sound_stream_start(&player.ring,SOUND_STREAM_PCM16,0,
                                          player.frames-player.position,1.0f,
                                          clip_done,(void *)(uintptr_t)player_seq);
            if(id<0) { player_halt(); player_set_state(P_ERROR); }
            else player.stream=id;
        } else if(++player.prime_waits>PLAYER_PRIME_FRAMES) {
            // The decoder never published and never said it was finished. Saying
            // so beats a player that reports "playing" in silence for ever.
            ESP_LOGE("pocket.av","the decoder produced nothing in %u frames",
                     (unsigned)PLAYER_PRIME_FRAMES);
            player.priming=false;
            player_halt();
            player_set_state(P_ERROR);
        }
    }
    uint32_t done=(uint32_t)atomic_load(&player_done_seq);
    if(player.stream&&done==player_seq) {
        atomic_store(&player_done_seq,0);
        bool ok=atomic_load(&player_done_ok);
        player.underruns+=sound_stream_underruns();
        // The audio task cannot tell a stream that ended from one whose decoder
        // gave up -- both look like a PCM ring that reached eof -- so the fault
        // count is what separates them. Without this a refused packet would be
        // reported to the app as a clip that simply finished early.
        if(player.codec==C_OPUS&&opus_feed_faults()) ok=false;
        if(player.codec==C_MP3) {
            if(mp3_feed_faults()||player.source_fault) ok=false;
            player.frames=mp3_feed_frames();
        }
        player.stream=0;
        if(ok) player.position=player.frames;
        if(player.state==P_PLAYING) player_set_state(ok?P_ENDED:P_ERROR);
    }
    // The refill, and the reason this surface has a pump at all now. Ahead of
    // the delivery below so a listener that runs long costs the ring nothing.
    if(player.state==P_PLAYING) player_feed();
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
                JS_NewString(ctx,player.codec==C_MP3?PLAYER_CODEC_MP3
                                :player.codec==C_OPUS?PLAYER_CODEC_OPUS
                                :player.codec==C_IMA?PLAYER_CODEC_IMA
                                                    :PLAYER_CODEC_PCM));
            JS_SetPropertyStr(ctx,info,"sampleRate",JS_NewInt32(ctx,SOUND_SAMPLE_RATE));
            JS_SetPropertyStr(ctx,info,"channels",JS_NewInt32(ctx,1));
            // MP3 answers from its own tag when it has one, because `frames`
            // is deliberately not a length there (see the field). null is the
            // honest answer for a file that never said, and it is a different
            // thing from 0.
            JS_SetPropertyStr(ctx,info,"durationMs",
                player.codec==C_MP3
                  ? (player.mp3_duration_ms?JS_NewInt32(ctx,(int)player.mp3_duration_ms)
                                           :JS_NULL)
                  : (player.frames==PLAYER_MP3_UNKNOWN?JS_NULL:
                     JS_NewInt32(ctx,(int)((uint64_t)player.frames*1000u/SOUND_SAMPLE_RATE))));
            // A network source has no index and no way back to a byte already
            // received, so it says so rather than accepting a seek it would
            // have to fake.
            JS_SetPropertyStr(ctx,info,"seekable",
                              (player.net||player.codec==C_MP3)?JS_FALSE:JS_TRUE);
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
                if(!strcmp(why,"nomem")) {
                    // The two numbers, in the message, the way pocket_net.c
                    // does it for a handshake. Playback needs about 45.6 KiB of
                    // which 18,436 and 14,336 are single blocks, so free alone
                    // does not say whether it would have fitted -- and a
                    // refusal that omits the largest block is a refusal nobody
                    // can act on.
                    char said[104];
                    snprintf(said,sizeof said,
                             "no room for the stream: %u free, %u largest block",
                             (unsigned)heap_caps_get_free_size(AV_HEAP_POOL),
                             (unsigned)heap_caps_get_largest_free_block(AV_HEAP_POOL));
                    return pocket_api_reject(ctx,POCKET_ERR_OUT_OF_MEMORY,op,
                                             said,true,POCKET_OUTCOME_NOT_APPLIED);
                }
                return pocket_api_reject(ctx,
                    !strcmp(why,"busy")?POCKET_ERR_BUSY:POCKET_ERR_NOT_AVAILABLE,
                    op,!strcmp(why,"busy")?"the sound queue is full"
                                          :"there is no room for the stream",
                    true,POCKET_OUTCOME_NOT_APPLIED);
            }
            player_set_state(P_PLAYING);
            // Section 9.1 has play() resolve on the output being accepted, not
            // on the stream being over; that is what onState is for.
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
            if(player.codec==C_MP3)
                return pocket_api_reject(ctx,POCKET_ERR_NOT_AVAILABLE,op,
                    "MP3 seeking requires an index and is not supported",false,
                    POCKET_OUTCOME_NOT_APPLIED);
            if(player.net)
                return pocket_api_reject(ctx,POCKET_ERR_NOT_AVAILABLE,op,
                    "a network stream cannot be seeked",false,
                    POCKET_OUTCOME_NOT_APPLIED);
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
                // The stream could not be requeued, so the position moved but
                // the sound stopped. outcome=applied is the honest half of that.
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
                JS_NewInt32(ctx,(int)((uint64_t)player_frames_now()*1000u/SOUND_SAMPLE_RATE)));
            // No longer always zero, and no longer honestly zero either: the
            // producer is the pump on the drawing task and it can be late. One
            // count is 128 frames, 5.3 ms of silence spliced in rather than
            // audio dropped -- so a nonzero reading means the sound got longer,
            // not that part of it went missing. Accumulated across pause and
            // seek because it is a property of the playback, not of the run.
            // Reported apart from underruns on purpose: a stall is the socket
            // being slow while the ring still had audio in it, so nothing was
            // heard. An underrun is audio that was owed and not delivered. Only
            // a stall long enough to drain the ring becomes one, and then both
            // numbers move. Collapsing them would make a healthy stream on a
            // twitchy network look like a broken decoder.
            if(player.net)
                JS_SetPropertyStr(ctx,status,"stalls",
                                  JS_NewUint32(ctx,opus_net_stalls()));
            JS_SetPropertyStr(ctx,status,"underruns",
                JS_NewInt32(ctx,(int)(player.underruns+
                    (player.stream?sound_stream_underruns():0))));
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

// The handle an open hands back. Split out of js_player_open because a network
// source settles LATER, from the pump, and has to build the identical object --
// an app must not be able to tell where its audio comes from by looking at the
// methods it was given.
static JSValue player_handle(JSContext *ctx) {
    JSValue object=JS_NewObject(ctx);
    if(JS_IsException(object)) return object;
    JSValue key=JS_NewInt32(ctx,player.id);
    player_add(ctx,object,"info",0,M_INFO,key);
    player_add(ctx,object,"play",0,M_PLAY,key);
    player_add(ctx,object,"pause",0,M_PAUSE,key);
    player_add(ctx,object,"seek",2,M_SEEK,key);
    player_add(ctx,object,"status",0,M_STATUS,key);
    player_add(ctx,object,"onState",1,M_ON_STATE,key);
    player_add(ctx,object,"close",0,M_CLOSE,key);
    JS_FreeValue(ctx,key);
    return object;
}

// ---- opening a network source
//
// The only asynchronous open this surface has, and it is asynchronous for a
// reason that cannot be designed away: the container header is the first bytes
// of the response, so there is nothing to check until the socket has answered.
// A file states its length before anything is read; a stream does not exist
// until it is flowing.
//
// So open() starts the receiver and arms a promise, and player_pump() settles it
// when the header has arrived or the receiver has given up. The app sees the
// same shape either way -- a promise that resolves to a player -- which keeps
// `source` a string an app can change rather than a branch it has to take.
#define PLAYER_NET_OPEN_MS 15000

static JSValue open_net_finish(JSContext *ctx, void *user, int32_t status,
                               const char *stop_code, bool *rejected) {
    (void)user;
    player.open_req=0;
    if(stop_code||status!=POCKET_STATUS_OK) {
        const char *why=opus_net_error();
        *rejected=true;
        JSValue error=pocket_api_error(ctx,
            stop_code?stop_code:POCKET_ERR_IO_ERROR,"audio.player.open",
            stop_code?"the open was stopped"
                     :(why?why:"the stream could not be opened"),
            false,POCKET_OUTCOME_NOT_APPLIED);
        player_teardown();
        return error;
    }
    *rejected=false;
    JSValue object=player_handle(ctx);
    if(JS_IsException(object)) player_teardown();
    return object;
}

static void open_net_stop(void *user, const char *code) {
    (void)user; (void)code;
    // Only asks. The completion still has to land before the slot is free, and
    // the teardown that frees the ring happens in the settle above -- freeing it
    // here would pull the buffer out from under a task that is still reading.
    opus_net_stop();
}

static const pocket_promise_ops_t open_net_ops = {
    .settle=open_net_finish, .stop=open_net_stop,
};

static JSValue open_net(JSContext *ctx, const char *OP, av_options_t *options) {
    // The packet ring is taken HERE rather than at play(), because for a network
    // source the receiver starts filling it now -- the header is in it. That is
    // the one place a network player costs more at rest than a file one: 6,144
    // bytes from open to close instead of from play to close.
    player.pkt_bytes=malloc(PLAYER_PKT_BYTES);
    if(!player.pkt_bytes) {
        JS_FreeValue(ctx,options->cancel);
        free(player.path); player.path=NULL;
        char said[104];
        snprintf(said,sizeof said,
                 "no room for the packet ring: %u free, %u largest block",
                 (unsigned)heap_caps_get_free_size(AV_HEAP_POOL),
                 (unsigned)heap_caps_get_largest_free_block(AV_HEAP_POOL));
        return pocket_api_reject(ctx,POCKET_ERR_OUT_OF_MEMORY,OP,said,true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    player.pkt.bytes=player.pkt_bytes;
    sound_stream_rewind(&player.pkt);

    pocket_request_t request=pocket_api_promise_open();
    if(!request) {
        JS_FreeValue(ctx,options->cancel);
        free(player.pkt_bytes); player.pkt_bytes=NULL; player.pkt.bytes=NULL;
        free(player.path); player.path=NULL;
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
            "too many operations are pending",true,POCKET_OUTCOME_NOT_APPLIED);
    }
    opus_net_start_t began=opus_net_start(player.path,&player.pkt);
    if(began!=OPUS_NET_OK) {
        pocket_api_promise_abandon(request);
        JS_FreeValue(ctx,options->cancel);
        free(player.pkt_bytes); player.pkt_bytes=NULL; player.pkt.bytes=NULL;
        free(player.path); player.path=NULL;
        return pocket_api_reject(ctx,
            began==OPUS_NET_NOMEM?POCKET_ERR_OUT_OF_MEMORY
                                 :began==OPUS_NET_BUSY?POCKET_ERR_BUSY
                                                      :POCKET_ERR_INVALID_ARGUMENT,
            OP,
            began==OPUS_NET_NOMEM?"no room for the receive task"
                                 :began==OPUS_NET_BUSY?"a stream is already being received"
                                                      :"source must be an http:// or https:// url",
            began!=OPUS_NET_BAD_URL,POCKET_OUTCOME_NOT_APPLIED);
    }
    // Open enough to be torn down, not open enough to be used: the handle that
    // carries the methods is not built until the settle, so nothing can call one.
    player.open=true; player.net=true; player.id=player_next_id++;
    player.codec=C_OPUS; player.block=0; player.per_block=1;
    player.position=0; player_reported=0; player.stream=0;
    player.feed=0; player.underruns=0; player.frames=0;
    player.offset=0; player.bytes=0;
    player.ring_bytes=NULL; player.ring.bytes=NULL;
    sound_stream_rewind(&player.ring);
    player.state=P_READY; player.announce=false;

    int64_t deadline=esp_timer_get_time()+
        (int64_t)(options->timeout_ms?options->timeout_ms:PLAYER_NET_OPEN_MS)*1000;
    JSValue promise=pocket_api_promise_arm(ctx,request,&open_net_ops,NULL,
                                           options->cancel,deadline);
    if(JS_IsException(promise)) { opus_net_stop(); player_teardown(); return promise; }
    player.open_req=request;
    return promise;
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
    // A network open really does wait, so it is the one that keeps the token and
    // polls it. A file open still settles inside this call and cannot observe a
    // cancellation that has not happened yet.
    bool net=!strncmp(source,"http://",7)||!strncmp(source,"https://",8);
    if(!net) JS_FreeValue(ctx,options.cancel);
    if(options.cancelled) {
        if(net) JS_FreeValue(ctx,options.cancel);
        JS_FreeCString(ctx,source);
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,
            "cancelled before the open",false,POCKET_OUTCOME_NOT_APPLIED);
    }

    // The path is what the player keeps instead of the bytes: every refill
    // re-resolves it, which is also what makes a source that is deleted or
    // replaced under a running stream end as a read failure rather than as a
    // stale buffer. One small allocation for the length of the player.
    size_t n=strlen(source)+1;
    char *path=malloc(n);
    if(path) memcpy(path,source,n);
    JS_FreeCString(ctx,source);
    if(!path)
        return pocket_api_reject(ctx,POCKET_ERR_OUT_OF_MEMORY,OP,
            "no room for the source path",true,POCKET_OUTCOME_NOT_APPLIED);
    player.path=path;
    player.net=false;
    player.open_req=0;
    if(net) return open_net(ctx,OP,&options);

    // The size first, and it is the only thing open() reads whole: nothing else
    // about this call scales with the file.
    const char *code=NULL;
    int32_t size=pocket_fs_read_all(player.path,NULL,0,&code);
    if(size<0) {
        JSValue error=pocket_api_reject(ctx,code?code:POCKET_ERR_IO_ERROR,OP,
                                        "the source could not be read",
                                        false,POCKET_OUTCOME_NOT_APPLIED);
        free(player.path); player.path=NULL;
        return error;
    }
    const char *why=source_parse((uint32_t)size);
    if(why) {
        free(player.path); player.path=NULL;
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,why,false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    player.open=true; player.id=player_next_id++;
    player.position=0; player_reported=0; player.stream=0;
    player.feed=0; player.underruns=0;
    player.ring_bytes=NULL; player.ring.bytes=NULL;
    player.pkt_bytes=NULL; player.pkt.bytes=NULL;
    sound_stream_rewind(&player.ring);
    sound_stream_rewind(&player.pkt);
    player.state=P_READY; player.announce=false;

    JSValue object=player_handle(ctx);
    if(JS_IsException(object)) { player_teardown(); return object; }
    // Section 9.1: open takes the header check and the resources, and does not
    // start the sound. What it no longer takes is the sound itself.
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
     .text=PLAYER_CODEC_PCM "," PLAYER_CODEC_IMA "," PLAYER_CODEC_OPUS "," PLAYER_CODEC_MP3},
    {.name="mp3Container", .kind=POCKET_LIMIT_TEXT,.text="mpeg-layer3"},
    {.name="mp3Seekable", .kind=POCKET_LIMIT_FLAG,.number=0},
    // The Opus entry is "opus/celt" and not "opus", and that is the honest
    // width of it: the decoder in this image will refuse a SILK or a hybrid
    // packet by name, because the 10,420-byte stack it is given was measured
    // for CELT 20 ms mono and is only a LOWER bound for the other modes. A
    // capability string that said "opus" would be publishing a stack nobody has
    // measured. The container is this project's own -- tools/make_opus_asset.py
    // writes it, main/pocket/opus_feed.h documents it -- and not Ogg.
    {.name="opusContainer", .kind=POCKET_LIMIT_TEXT,.text="pocket/opus-packets-1"},
    {.name="opusFrameMs",   .kind=POCKET_LIMIT_INT, .number=20},
    {.name="volumes",        .kind=POCKET_LIMIT_TEXT,.text="app,assets"},
    {.name="sampleRate",     .kind=POCKET_LIMIT_INT,.number=(int32_t)SOUND_SAMPLE_RATE},
    {.name="channels",       .kind=POCKET_LIMIT_INT,.number=1},
    // maxSourceBytes is GONE, and its absence is the report. It used to say
    // 24,576, which this file enforced with a malloc; nothing enforces a source
    // size here any more, and a limit nothing enforces is worse than no limit.
    // What still bounds a source is the volume it sits on -- `app:` publishes
    // maxFileBytes on fs.volume.app, which is where an app should have been
    // asking all along.
    {.name="concurrent",     .kind=POCKET_LIMIT_INT,.number=1},
    {.name="maxWatches",     .kind=POCKET_LIMIT_INT,.number=PLAYER_WATCHES},
    {.name="seekable",       .kind=POCKET_LIMIT_FLAG,.number=1},
    // An ADPCM clip resumes where a block begins, so a seek lands on the block
    // containing the request. 256-byte blocks at 24 kHz are 21 ms; the number
    // is the file's, so this states the shape rather than a value.
    {.name="seekBlockAligned",.kind=POCKET_LIMIT_FLAG,.number=1},
    {.name="streaming",      .kind=POCKET_LIMIT_FLAG,.number=1},
    // What an underrun does, published because an app can hear the difference
    // and cannot otherwise tell which of the three it got. "stretch" is
    // silence inserted with nothing dropped and nothing repeated, and
    // positionMs stalling for exactly as long; the alternatives would have been
    // "drop" (audio lost to hold timing) and "stop". The full argument is above
    // sound_stream_start() in sound.h.
    {.name="underrunPolicy", .kind=POCKET_LIMIT_TEXT,.text="stretch"},
    // And the one case that is not survivable: a producer that publishes
    // nothing at all for this long ends the stream, and onState reports it the
    // way it reports a failed I2S write.
    {.name="maxStarveMs",    .kind=POCKET_LIMIT_INT,
     .number=(int32_t)SOUND_STREAM_STARVE_MS},
    // What the ring costs while a player is playing, and the ceiling on an
    // ADPCM block that follows from it: a block has to fit one slot whole or a
    // slot boundary lands where the predictor cannot be reseeded. wav_parse
    // refuses a larger block by name.
    {.name="ringBytes",      .kind=POCKET_LIMIT_INT,.number=PLAYER_RING_BYTES},
    {.name="maxBlockBytes",  .kind=POCKET_LIMIT_INT,
     .number=(int32_t)SOUND_STREAM_SLOT_BYTES},
    // A second ring of the same size, and only while an Opus source is playing:
    // the pump fills it with packets and the decode task drains it. Published
    // because it is playtime heap an app can be told about rather than discover.
    {.name="opusRingBytes",  .kind=POCKET_LIMIT_INT,.number=PLAYER_PKT_BYTES},
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
