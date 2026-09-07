#include "pocket_app.h"
#include "pocket_api.h"
#include "app_session.h"
#include "jsconsole.h"
#include "solar_time.h"
#include "utf8.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "pocket.app";

// Four listeners is what one screen's worth of animation wants; a program with
// five views of the clock wants one listener and its own fan-out, the same rule
// pocket_imu.c applies to watches.
#define APP_FRAME_LISTENERS 4
// Section 5's "最大200ms" for the stop hook, as an initial figure.
#define APP_STOP_MS         200
// Section 14 caps a general Promise at 30000ms and audio.tone and storage.kv
// both enforce that number, so sleep uses it too rather than inventing a third.
#define APP_MAX_TIMEOUT_MS  30000
#define APP_SLEEP_SLACK_MS  1000
// The shared completion table in pocket_api.c is four deep and audio.tone wants
// one of them, so sleep takes two and leaves room rather than filling it.
#define APP_SLEEPS          2

static JSContext *js_ctx;               // the realm this surface installed into

// ------------------------------------------------------------------ lifecycle
//
// Section 5's states, and the one place that says which is which. Loading is
// source evaluation, the only moment pocket.app.start() may be called in. The
// first pump after that runs the start hook; onFrame is delivered in Running
// only, so a program whose start hook is still awaiting something gets its I/O
// completions -- the pumps keep running -- and no frames.

typedef enum { PHASE_LOADING, PHASE_STARTING, PHASE_RUNNING, PHASE_STOPPED } phase_t;

static phase_t phase;
static bool    registered;      // pocket.app.start() was called
static JSValue start_hook, stop_hook;
static int64_t frame_last_us;   // 0 until the first frame

static pocket_sub_slot_t  frame_slots[APP_FRAME_LISTENERS];
static pocket_sub_table_t frame_table = {
    .slots=frame_slots, .count=APP_FRAME_LISTENERS,
    .tag="pocket.app", .what="onFrame",
    // Same rule the two polled surfaces use: a listener that throws every frame
    // would otherwise fill the log and keep costing a call for the life of the
    // program. The program keeps its other listeners.
    .close_on_throw=true,
};

// fps for device.metrics(), measured where the frames actually arrive. Null
// until a full window has passed, because a figure from a third of a second is
// not the number the name promises.
static int64_t  fps_window_us;
static unsigned fps_frames;
static double   fps_value = -1.0;

// --------------------------------------------------------------- the log ring
//
// Section 7's ring, with the two ceilings it names -- 256 UTF-8 bytes a record,
// 32 records -- enforced exactly. The text does not sit in 32 fixed 256-byte
// slots, though: that is 8KiB of a 334KiB DRAM budget standing empty for the
// programs that log a line of forty characters. The bytes live in one arena and
// a record is evicted when either ceiling is reached, so the cost is what was
// actually written. A reader sees an eviction as a gap in `sequence`, which is
// how it would see one from the 32-record ceiling too.
//
// Nothing outside this file can write here. That is deliberate, and it is what
// section 3's "認証情報を含む保存値やWi-Fiパスワードをアプリの通常ログへ出さない"
// needs: the only producer is the guest itself, so a passphrase can be in this
// ring only if the program already had it, and read() hands a program its own
// records and no others. wifi_time.c wipes its passphrase before the radio goes
// down and has no way to reach this ring even while it holds one.

#define LOG_RECORDS       32
#define LOG_RECORD_BYTES  256
#define LOG_ARENA_BYTES   1536
// Enough for a program that prints one line a frame, and far below what a
// print loop produces. The arena is 1536 bytes, so this is also roughly the
// point past which a record could not survive one second in the ring anyway.
#define LOG_BYTES_PER_SEC 4096
#define LOG_READ_MAX      8

typedef struct {
    uint32_t sequence;
    uint32_t time_ms;
    uint16_t offset;        // into the arena, wrapping
    uint16_t length;
    uint8_t  level;
    bool     truncated;
} log_record_t;

static const char *const LOG_LEVELS[] = { "debug", "info", "warn", "error" };

// The ring is taken on the first line an app writes and given back when the
// app ends. Two kilobytes of .bss otherwise sit there for every program,
// including the ones that never print -- the same reasoning pocket_fs.c gives
// for its index, and the same shape: one allocation, freed in the reset.
typedef struct {
    log_record_t records[LOG_RECORDS];
    char         arena[LOG_ARENA_BYTES];
} log_ring_t;

static log_ring_t  *log_ring;
static unsigned     log_head, log_count;    // ring of records
static unsigned     log_write, log_used;    // ring of bytes
static uint32_t     log_sequence;
static uint32_t     log_dropped;            // refused by the per-second budget
static int64_t      log_window_us;
static unsigned     log_window_bytes;

static void log_reset(void) {
    free(log_ring);
    log_ring=NULL;
    log_head=log_count=log_write=log_used=0;
    log_sequence=0; log_dropped=0;
    log_window_us=0; log_window_bytes=0;
}

static void log_evict_oldest(void) {
    if(!log_count) return;
    log_used-=log_ring->records[log_head].length;
    log_head=(log_head+1)%LOG_RECORDS;
    log_count--;
}

// Returns false only when the ring could not be taken. A refused record is
// otherwise a dropped one, which read().dropped already accounts for; the
// caller that asked for this write by name is the one that turns false into an
// error, and console.log is not it.
static bool log_record(int level, const char *text, size_t length) {
    if(!log_ring) {
        log_ring=calloc(1,sizeof(*log_ring));
        if(!log_ring) return false;
    }
    // Truncate on a character boundary: half a UTF-8 sequence read back through
    // read() would be a string the program cannot print.
    bool truncated=false;
    if(length>LOG_RECORD_BYTES) {
        length=LOG_RECORD_BYTES;
        while(length && utf8_is_cont(text[length])) length--;
        truncated=true;
    }
    // Section 7's per-second byte budget. A program in a print loop loses the
    // overflow and is told how much through read().dropped, rather than having
    // its own older records pushed out by its newest ones.
    int64_t now=esp_timer_get_time();
    if(now-log_window_us>=1000000) { log_window_us=now; log_window_bytes=0; }
    if(log_window_bytes+length>LOG_BYTES_PER_SEC) { log_dropped++; return true; }
    log_window_bytes+=(unsigned)length;

    while(log_count>=LOG_RECORDS || log_used+length>LOG_ARENA_BYTES)
        log_evict_oldest();
    unsigned offset=log_write;
    size_t   first=length;
    if(offset+first>LOG_ARENA_BYTES) first=LOG_ARENA_BYTES-offset;
    memcpy(log_ring->arena+offset,text,first);
    memcpy(log_ring->arena,text+first,length-first);
    log_write=(unsigned)((offset+length)%LOG_ARENA_BYTES);
    log_used+=(unsigned)length;

    unsigned slot=(log_head+log_count)%LOG_RECORDS;
    log_ring->records[slot]=(log_record_t){
        .sequence=++log_sequence,
        .time_ms=(uint32_t)(now/1000),
        .offset=(uint16_t)offset, .length=(uint16_t)length,
        .level=(uint8_t)level, .truncated=truncated,
    };
    log_count++;
    return true;
}

static JSValue log_text(JSContext *ctx, const log_record_t *r) {
    char   buffer[LOG_RECORD_BYTES];
    size_t first=r->length;
    if(r->offset+first>LOG_ARENA_BYTES) first=LOG_ARENA_BYTES-r->offset;
    memcpy(buffer,log_ring->arena+r->offset,first);
    memcpy(buffer+first,log_ring->arena,r->length-first);
    return JS_NewStringLen(ctx,buffer,r->length);
}

static JSValue js_log_write(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv) {
    (void)this_val;
    static const char *const OP="log.write";
    const char *level=argc>0?JS_ToCString(ctx,argv[0]):NULL;
    int kind=-1;
    if(level) {
        for(int i=0;i<(int)(sizeof(LOG_LEVELS)/sizeof(LOG_LEVELS[0]));i++)
            if(!strcmp(level,LOG_LEVELS[i])) { kind=i; break; }
        JS_FreeCString(ctx,level);
    }
    if(kind<0)
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                "level must be debug, info, warn or error",false,NULL);
    // A number or an object here is a mistake worth naming rather than a silent
    // "[object Object]" in the ring; console.log is the surface that takes any
    // value and stringifies it.
    if(argc<2 || !JS_IsString(argv[1]))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                "write(level, text) needs a string",false,NULL);
    size_t      length=0;
    const char *text=JS_ToCStringLen(ctx,&length,argv[1]);
    if(!text) return JS_EXCEPTION;
    bool kept=log_record(kind,text,length);
    JS_FreeCString(ctx,text);
    // The ring is taken on the first write, so this is where running out of
    // room to keep a log reaches the program that asked for one.
    if(!kept) return pocket_api_throw(ctx,POCKET_ERR_OUT_OF_MEMORY,OP,
                                      "no memory for the log ring",true,
                                      POCKET_OUTCOME_NOT_APPLIED);
    // Not echoed to USB: section 7 asks the log path not to stop a JS turn on a
    // synchronous full-volume write. read() is how a record leaves the device.
    return JS_UNDEFINED;
}

static JSValue js_log_read(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv) {
    (void)this_val;
    static const char *const OP="log.read";
    double after=0, limit=LOG_READ_MAX;
    if(argc>0 && JS_IsObject(argv[0])) {
        static const char *const FIELDS[]={"afterSequence","limit"};
        double *const SLOTS[]={&after,&limit};
        for(int i=0;i<2;i++) {
            JSValue field=JS_GetPropertyStr(ctx,argv[0],FIELDS[i]);
            if(JS_IsException(field)) return JS_EXCEPTION;
            if(JS_IsUndefined(field)||JS_IsNull(field)) { JS_FreeValue(ctx,field); continue; }
            bool bad=!JS_IsNumber(field)||JS_ToFloat64(ctx,SLOTS[i],field);
            JS_FreeValue(ctx,field);
            if(bad||!isfinite(*SLOTS[i])||*SLOTS[i]<0)
                return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                        "afterSequence and limit must be positive numbers",
                                        false,NULL);
        }
    } else if(argc>0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0]))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                "read(options) needs an object",false,NULL);
    // Section 7 caps one read at 8 records. Asking for more is not refused --
    // the cap is the contract, not a limit the caller broke -- but 8 is what
    // comes back, and the last record's sequence is how the caller asks for the
    // next page.
    if(limit>LOG_READ_MAX) limit=LOG_READ_MAX;

    JSValue result=JS_NewObject(ctx);
    if(JS_IsException(result)) return result;
    JSValue records=JS_NewArray(ctx);
    uint32_t taken=0;
    for(unsigned i=0;i<log_count && (double)taken<limit;i++) {
        const log_record_t *r=&log_ring->records[(log_head+i)%LOG_RECORDS];
        if((double)r->sequence<=after) continue;
        JSValue entry=JS_NewObject(ctx);
        JS_SetPropertyStr(ctx,entry,"sequence",JS_NewUint32(ctx,r->sequence));
        JS_SetPropertyStr(ctx,entry,"timeMs",JS_NewUint32(ctx,r->time_ms));
        JS_SetPropertyStr(ctx,entry,"level",JS_NewString(ctx,LOG_LEVELS[r->level]));
        JS_SetPropertyStr(ctx,entry,"text",log_text(ctx,r));
        JS_SetPropertyStr(ctx,entry,"truncated",JS_NewBool(ctx,r->truncated));
        JS_SetPropertyUint32(ctx,records,taken++,entry);
    }
    JS_SetPropertyStr(ctx,result,"records",records);
    // Cumulative since the session started, so two reads of the same ring agree
    // and a program can tell whether anything was lost between them.
    JS_SetPropertyStr(ctx,result,"dropped",JS_NewUint32(ctx,log_dropped));
    return result;
}

// print / console.log / console.warn / console.error. Section 7 puts them on
// the same path as log.write, and the original still runs afterwards with the
// arguments it was given, so jsconsole.c's screen ring and USB line are exactly
// what they were. The values are stringified a second time here rather than
// joined into one argument and passed on: quickjs-ng exports no string concat,
// and building the join in JS to save a conversion would cost an allocation per
// call in the heap that is actually scarce.
//
// One byte past the record ceiling is deliberate: it is what makes log_record()
// see an over-long line, cut it on a character boundary and mark it truncated.
static JSValue js_log_console(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int magic,
                              JSValueConst *func_data) {
    (void)this_val;
    char   joined[LOG_RECORD_BYTES+8];
    size_t used=0;
    for(int i=0;i<argc && used<=LOG_RECORD_BYTES;i++) {
        if(i) joined[used++]=' ';
        size_t      length=0;
        const char *text=JS_ToCStringLen(ctx,&length,argv[i]);
        if(!text) return JS_EXCEPTION;
        size_t room=sizeof(joined)-used;
        if(length>room) length=room;
        memcpy(joined+used,text,length);
        used+=length;
        JS_FreeCString(ctx,text);
    }
    log_record(magic,joined,used);
    return JS_Call(ctx,func_data[0],JS_UNDEFINED,argc,argv);
}

// Replaces one console function with the wrapper above, keeping the original as
// the wrapper's own data. A name that is missing is left alone.
static void wrap_console(JSContext *ctx, JSValueConst object,
                         const char *name, int level) {
    JSValue original=JS_GetPropertyStr(ctx,object,name);
    if(!JS_IsFunction(ctx,original)) { JS_FreeValue(ctx,original); return; }
    JS_SetPropertyStr(ctx,object,name,
        JS_NewCFunctionData(ctx,js_log_console,1,level,1,&original));
    JS_FreeValue(ctx,original);
}

// ------------------------------------------------------------------- metrics
//
// Section 7 asks for cached values. The cache is here rather than in the pump
// because a program that never calls metrics() should pay nothing for it, and
// JS_ComputeMemoryUsage walks the object list -- that is not a per-frame cost.

#define METRICS_CACHE_US 500000

static JSValue js_metrics(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    static int64_t  taken_us;
    static unsigned heap_free, heap_largest;
    static double   js_heap;
    int64_t now=esp_timer_get_time();
    if(!taken_us || now-taken_us>=METRICS_CACHE_US) {
        taken_us=now;
        heap_free=(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        heap_largest=(unsigned)heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        JSMemoryUsage usage;
        memset(&usage,0,sizeof(usage));
        JS_ComputeMemoryUsage(JS_GetRuntime(ctx),&usage);
        js_heap=(double)usage.malloc_size;
    }
    JSValue object=JS_NewObject(ctx);
    if(JS_IsException(object)) return object;
    JS_SetPropertyStr(ctx,object,"heapFreeBytes",JS_NewUint32(ctx,heap_free));
    JS_SetPropertyStr(ctx,object,"largestFreeBlockBytes",JS_NewUint32(ctx,heap_largest));
    JS_SetPropertyStr(ctx,object,"jsHeapBytes",JS_NewFloat64(ctx,js_heap));
    JS_SetPropertyStr(ctx,object,"fps",fps_value>=0?JS_NewFloat64(ctx,fps_value):JS_NULL);
    // Null, not zero: section 7 wants an unobtainable value to say so, and this
    // host counts no dropped I/O anywhere. The one counter that does exist --
    // the log's own -- is reported by log.read(), where it belongs.
    JS_SetPropertyStr(ctx,object,"ioDropped",JS_NULL);
    return object;
}

// ---------------------------------------------------------------------- time

static JSValue js_time_now(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    // Section 4's monotonic milliseconds, the same clock every other timeMs on
    // this host is read from.
    return JS_NewFloat64(ctx,esp_timer_get_time()/1000.0);
}

// J2000 noon in Unix seconds; solar_time.c counts its days from there.
#define J2000_UNIX_SECONDS 946728000.0

static JSValue js_time_wall(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    // The trust decision stays in solar_time.c, which reports a UTC source only
    // after a successful SNTP sync set the clock. Its demo epoch is a number
    // for the sail to animate with and not a wall clock, so it comes back here
    // as unixMs null -- section 5 types it that way, and a program that shows a
    // date has to be able to tell the two apart. No timezone is applied: that
    // is reserved to a future ephemeris provider, not to this layer.
    solar_time_sample_t sample=solar_time_now(0);
    JSValue object=JS_NewObject(ctx);
    if(JS_IsException(object)) return object;
    bool utc=sample.source==SOLAR_TIME_UTC;
    JS_SetPropertyStr(ctx,object,"unixMs",
        utc?JS_NewFloat64(ctx,round((sample.days*86400.0+J2000_UNIX_SECONDS)*1000.0))
           :JS_NULL);
    // "host" is never returned: nothing on this device sets the clock by hand,
    // so a synchronised clock came from the network and an unsynchronised one
    // is unsynced. The third value stays in the type for a host that gains a
    // settable clock.
    JS_SetPropertyStr(ctx,object,"source",JS_NewString(ctx,utc?"network":"unsynced"));
    return object;
}

// ------------------------------------------------------------------- sleep
//
// The driver is the pump below, so `stop` has to post the completion itself:
// pocket_api_pump() asks a stopped request's driver to stop and then waits for
// that driver's completion, and there is no other task here to send one.

typedef struct {
    bool             active;
    bool             elapsed;    // the full time passed before anything stopped it
    pocket_request_t request;
    int64_t          due_us;
} sleep_t;

static sleep_t  sleeps[APP_SLEEPS];
static unsigned sleeps_open;

// The only status sleep posts besides POCKET_STATUS_OK: it was stopped with
// time still to run, which is what tells settle() the wait was not applied.
#define SLEEP_STATUS_EARLY 1

static sleep_t *sleep_of(void *user) { return &sleeps[(uintptr_t)user-1]; }

static void sleep_stop(void *user, const char *code) {
    (void)code;
    sleep_t *s=sleep_of(user);
    pocket_api_complete(s->request,s->elapsed?POCKET_STATUS_OK:SLEEP_STATUS_EARLY);
}

static JSValue sleep_settle(JSContext *ctx, void *user, int32_t status,
                            const char *stop_code, bool *rejected) {
    (void)user;
    *rejected=stop_code!=NULL;
    if(!stop_code) return JS_UNDEFINED;
    return pocket_api_error(ctx,stop_code,"time.sleep",
                            !strcmp(stop_code,POCKET_ERR_TIMEOUT)
                                ?"timeoutMs expired before the sleep did"
                                :"cancelled while sleeping",
                            false,
                            status==POCKET_STATUS_OK?POCKET_OUTCOME_APPLIED
                                                    :POCKET_OUTCOME_NOT_APPLIED);
}

static void sleep_released(void *user) {
    sleep_of(user)->active=false;
    sleeps_open--;
}

static const pocket_promise_ops_t sleep_ops = {
    .settle=sleep_settle, .stop=sleep_stop, .release=sleep_released,
};

// Section 4 puts an argument error from a Promise-returning method into the
// rejection, so every exit here goes through pocket_api_reject().
static JSValue js_sleep(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv) {
    (void)this_val;
    static const char *const OP="time.sleep";
    double ms=0;
    if(argc<1 || !JS_IsNumber(argv[0]) || JS_ToFloat64(ctx,&ms,argv[0]) ||
       !isfinite(ms) || ms!=(double)(int64_t)ms || ms<0 || ms>APP_MAX_TIMEOUT_MS)
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "sleep(ms) needs a whole number of 0 to 30000",
                                 false,POCKET_OUTCOME_NOT_APPLIED);

    int32_t timeout_ms=0;
    JSValue cancel=JS_UNDEFINED;
    if(argc>1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        if(!JS_IsObject(argv[1]))
            return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                     "options must be an object",false,
                                     POCKET_OUTCOME_NOT_APPLIED);
        JSValue field=JS_GetPropertyStr(ctx,argv[1],"timeoutMs");
        if(JS_IsException(field)) return JS_EXCEPTION;
        if(!JS_IsUndefined(field) && !JS_IsNull(field)) {
            double value=0;
            bool bad=!JS_IsNumber(field)||JS_ToFloat64(ctx,&value,field);
            JS_FreeValue(ctx,field);
            // Section 4 refuses an over-range request rather than rounding it.
            if(bad||!isfinite(value)||value!=(double)(int64_t)value||
               value<1||value>APP_MAX_TIMEOUT_MS)
                return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                         "timeoutMs must be a whole number of 1 to 30000",
                                         false,POCKET_OUTCOME_NOT_APPLIED);
            timeout_ms=(int32_t)value;
        } else JS_FreeValue(ctx,field);

        cancel=JS_GetPropertyStr(ctx,argv[1],"cancel");
        if(JS_IsException(cancel)) return JS_EXCEPTION;
        if(JS_IsUndefined(cancel)||JS_IsNull(cancel)) {
            JS_FreeValue(ctx,cancel); cancel=JS_UNDEFINED;
        } else if(!pocket_api_is_cancel_token(cancel)) {
            JS_FreeValue(ctx,cancel);
            return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                     "cancel must be a token from pocket.cancel.source()",
                                     false,POCKET_OUTCOME_NOT_APPLIED);
        } else if(pocket_api_cancel_requested(cancel)) {
            JS_FreeValue(ctx,cancel);
            return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,
                                     "cancelled before the sleep",false,
                                     POCKET_OUTCOME_NOT_APPLIED);
        }
    }

    int free_slot=-1;
    for(int i=0;i<APP_SLEEPS;i++) if(!sleeps[i].active) { free_slot=i; break; }
    // Section 4 allows one operation of a kind at a time and answers the next
    // with BUSY; two is what this surface can hold without filling the
    // completion table the other surfaces share.
    if(free_slot<0) {
        JS_FreeValue(ctx,cancel);
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,"too many sleeps are pending",
                                 true,POCKET_OUTCOME_NOT_APPLIED);
    }
    pocket_request_t request=pocket_api_promise_open();
    if(!request) {
        JS_FreeValue(ctx,cancel);
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
                                 "too many operations are pending",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    int64_t now=esp_timer_get_time();
    sleeps[free_slot]=(sleep_t){
        .active=true, .request=request, .due_us=now+(int64_t)ms*1000,
    };
    // timeoutMs is measured from the call and has to leave room for the sleep
    // itself, so the default is the wait plus the same second of slack a queued
    // tone gets. A timeoutMs shorter than the sleep is not an error: the
    // program asked for a deadline it knows the sleep cannot meet, and TIMEOUT
    // is the honest answer to that.
    int64_t deadline_us=now+1000LL*(timeout_ms?timeout_ms:(int64_t)ms+APP_SLEEP_SLACK_MS);
    JSValue promise=pocket_api_promise_arm(ctx,request,&sleep_ops,
                                           (void *)(uintptr_t)(free_slot+1),
                                           cancel,deadline_us);
    // The slot is given back by hand here: arm() failed before ops were
    // installed, so no release hook runs for this call.
    if(JS_IsException(promise)) { sleeps[free_slot].active=false; return promise; }
    sleeps_open++;
    return promise;
}

// ---------------------------------------------------------------- app.onFrame

static bool frame_payload(JSContext *ctx, int slot, void *user, JSValue *payload) {
    (void)slot;
    const int64_t *times=user;
    JSValue object=JS_NewObject(ctx);
    if(JS_IsException(object)) return false;
    JS_SetPropertyStr(ctx,object,"timeMs",JS_NewFloat64(ctx,times[0]/1000.0));
    JS_SetPropertyStr(ctx,object,"deltaMs",JS_NewFloat64(ctx,times[1]/1000.0));
    *payload=object;
    return true;
}

// globalThis.frame, provided by the host. Section 5 asks the new runtime to
// supply it so that a program which only awaits Promises still keeps its event
// loop, and pocketjs_guest_eval latches whatever this name holds at the end of
// evaluation -- so a program that calls pocket.app.start() no longer has to
// write a frame function to be startable at all.
//
// It cannot return a Promise, which is what section 5's "frameは同期関数" was
// guarding against: under a host-owned frame that failure is structurally
// absent rather than detected. A listener's return value is dropped, the same
// way every other subscription on this host drops one.
static JSValue js_host_frame(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    int64_t now=esp_timer_get_time();
    int64_t times[2]={now,frame_last_us?now-frame_last_us:0};
    frame_last_us=now;
    if(!fps_window_us) fps_window_us=now;
    fps_frames++;
    if(now-fps_window_us>=1000000) {
        fps_value=fps_frames*1000000.0/(double)(now-fps_window_us);
        fps_window_us=now; fps_frames=0;
    }
    // Section 5: onFrame is delivered in Running only. During Starting the
    // pumps keep running, so I/O completions and cancellation still arrive.
    if(phase==PHASE_RUNNING) pocket_api_sub_deliver(&frame_table,frame_payload,times);
    return JS_UNDEFINED;
}

static JSValue js_on_frame(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv) {
    (void)this_val;
    static const char *const OP="app.onFrame";
    if(argc<1 || !JS_IsFunction(ctx,argv[0]))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                "onFrame(listener) needs a function",false,NULL);
    // Section 2 forbids one program mixing a legacy frame with the new frame
    // registration, and Running -- the only state onFrame is delivered in --
    // exists only once start() has been called.
    if(!registered)
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                "call pocket.app.start() before onFrame",false,NULL);
    return pocket_api_sub_open(ctx,&frame_table,argv[0],OP,
                               "too many frame listeners",NULL);
}

// ----------------------------------------------------------- app.start / exit

static JSValue take_hook(JSContext *ctx, JSValueConst hooks, const char *name,
                         JSValue *out, bool *bad) {
    JSValue hook=JS_GetPropertyStr(ctx,hooks,name);
    if(JS_IsException(hook)) { *bad=true; return JS_EXCEPTION; }
    if(JS_IsUndefined(hook)||JS_IsNull(hook)) { JS_FreeValue(ctx,hook); return JS_UNDEFINED; }
    if(!JS_IsFunction(ctx,hook)) {
        JS_FreeValue(ctx,hook);
        *bad=true;
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"app.start",
                                "start and stop must be functions",false,NULL);
    }
    *out=hook;
    return JS_UNDEFINED;
}

static JSValue js_start(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv) {
    (void)this_val;
    static const char *const OP="app.start";
    if(argc<1 || !JS_IsObject(argv[0]))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                "start(hooks) needs an object",false,NULL);
    // Section 5: registered once, during source evaluation. After that the
    // program is Starting or Running and a second set of hooks would have no
    // moment left to be called in.
    if(registered || phase!=PHASE_LOADING)
        return pocket_api_throw(ctx,POCKET_ERR_CONFLICT,OP,
                                "pocket.app.start() runs once, while the source loads",
                                false,POCKET_OUTCOME_NOT_APPLIED);

    JSValue global=JS_GetGlobalObject(ctx);
    JSValue existing=JS_GetPropertyStr(ctx,global,"frame");
    bool    mixed=JS_IsFunction(ctx,existing);
    JS_FreeValue(ctx,existing);
    if(mixed) {
        // Section 2's "同一アプリ内でlegacy frameと新しいフレーム登録を併用しない",
        // caught at the one moment it still can be. A frame assigned AFTER this
        // call replaces the host's and silently takes onFrame with it; the
        // property is left writable on purpose, because app_session.c's
        // FRAME_WRAP is what routes a runtime exception to the Playground's
        // error line and it works by replacing this same name.
        JS_FreeValue(ctx,global);
        return pocket_api_throw(ctx,POCKET_ERR_CONFLICT,OP,
                                "this program already defines globalThis.frame",
                                false,POCKET_OUTCOME_NOT_APPLIED);
    }
    bool    bad=false;
    JSValue error=take_hook(ctx,argv[0],"start",&start_hook,&bad);
    if(!bad) error=take_hook(ctx,argv[0],"stop",&stop_hook,&bad);
    if(bad) {
        JS_FreeValue(ctx,global);
        JS_FreeValue(ctx,start_hook); JS_FreeValue(ctx,stop_hook);
        start_hook=stop_hook=JS_UNDEFINED;
        return error;
    }
    JS_SetPropertyStr(ctx,global,"frame",
                      JS_NewCFunction(ctx,js_host_frame,"frame",0));
    JS_FreeValue(ctx,global);
    registered=true;
    return JS_UNDEFINED;
}

// Deferred to the next turn on purpose. app_request_stop() raises the flag the
// guest's interrupt handler answers, so calling it from here would cut the rest
// of this listener -- and the rest of this turn -- off mid-statement with an
// InternalError. The pump asks for the stop at a turn boundary instead, where
// there is no JS on the stack to interrupt.
//
// What follows is still not a clean exit, and this is the honest description of
// it: main.c ends a run on Back or on a failing app_tick and has no third path,
// so the stop lands as a failed turn and the run is reported as one. What that
// costs is the error line, which pocket_app_reset() rewrites to say the app
// exited. A real self-exit wants a line in main.c that this file does not own.
static bool exit_requested;

static JSValue js_exit(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    exit_requested=true;
    return JS_UNDEFINED;
}

// A hook's outcome. hook_pending is a Promise the caller must wait for, and
// hook_failed a hook that threw or rejected -- a program that did not start, or
// did not save. Either is reported the way a frame exception is.
static bool hook_pending, hook_failed;

static JSValue hook_done(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv, int magic,
                         JSValueConst *func_data) {
    (void)this_val; (void)func_data;
    hook_pending=false;
    if(magic) {
        hook_failed=true;
        const char *text=argc>0?JS_ToCString(ctx,argv[0]):NULL;
        ESP_LOGW(TAG,"hook rejected: %s",text?text:"?");
        if(text) { jsconsole_set_error(text); JS_FreeCString(ctx,text); }
    }
    return JS_UNDEFINED;
}

static void run_hook(JSContext *ctx, JSValue hook, JSValueConst argument, int argc) {
    hook_pending=false; hook_failed=false;
    JSValue result=JS_Call(ctx,hook,JS_UNDEFINED,argc,&argument);
    if(JS_IsException(result)) {
        JSValue exception=JS_GetException(ctx);
        const char *text=JS_ToCString(ctx,exception);
        ESP_LOGW(TAG,"hook threw: %s",text?text:"?");
        if(text) { jsconsole_set_error(text); JS_FreeCString(ctx,text); }
        JS_FreeValue(ctx,exception);
        hook_failed=true;
        return;
    }
    JSValue then=JS_IsObject(result)?JS_GetPropertyStr(ctx,result,"then"):JS_UNDEFINED;
    if(JS_IsException(then)) { JS_FreeValue(ctx,JS_GetException(ctx)); then=JS_UNDEFINED; }
    if(JS_IsFunction(ctx,then)) {
        // Section 5 lets a hook return a Promise, so the state it moves the app
        // into waits for that Promise rather than for the call.
        JSValue handlers[2]={
            JS_NewCFunctionData(ctx,hook_done,1,0,0,NULL),
            JS_NewCFunctionData(ctx,hook_done,1,1,0,NULL),
        };
        hook_pending=true;
        JSValue chained=JS_Call(ctx,then,result,2,(JSValueConst *)handlers);
        if(JS_IsException(chained)) {
            JS_FreeValue(ctx,JS_GetException(ctx));
            hook_pending=false;
            hook_failed=true;
        }
        JS_FreeValue(ctx,chained);
        JS_FreeValue(ctx,handlers[0]);
        JS_FreeValue(ctx,handlers[1]);
    }
    JS_FreeValue(ctx,then);
    JS_FreeValue(ctx,result);
}

// -------------------------------------------------------------- pump / reset

void pocket_app_pump(void) {
    // Before anything else in the turn, and before any JS runs in it.
    // Left standing rather than cleared: app_request_stop() only stores a flag,
    // and pocket_app_reset() reads this one to know the app ended on its own.
    if(exit_requested) app_request_stop();
    if(sleeps_open) {
        int64_t now=esp_timer_get_time();
        for(int i=0;i<APP_SLEEPS;i++)
            if(sleeps[i].active && !sleeps[i].elapsed && now>=sleeps[i].due_us) {
                sleeps[i].elapsed=true;
                // pocket_api_pump(), which runs next, is what settles it.
                pocket_api_complete(sleeps[i].request,POCKET_STATUS_OK);
            }
    }
    if(phase==PHASE_LOADING) {
        // Evaluation is over by the time the first turn runs, and that is what
        // closes the window pocket.app.start() may be called in.
        if(!registered) { phase=PHASE_STOPPED; return; }   // a legacy program
        phase=PHASE_STARTING;
        if(JS_IsUndefined(start_hook)) { phase=PHASE_RUNNING; return; }
        run_hook(js_ctx,start_hook,JS_UNDEFINED,0);
    }
    if(phase==PHASE_STARTING && !hook_pending) {
        // A start hook that failed leaves a program that never started, so it
        // is stopped rather than run without whatever the hook was setting up.
        if(hook_failed) app_request_stop();
        phase=PHASE_RUNNING;
    }
}

// The stop hook has to be able to run after the shell has already asked the
// guest to stop, and app_session.c's interrupt handler answers yes to every
// call from that moment on. Swapping in a handler with a deadline of its own is
// what gives section 5's 200ms budget somewhere to exist, and it also bounds a
// hook that loops forever. Nothing puts the old handler back: the guest is
// destroyed a few lines after this returns, and the next session installs its
// own from app_session.c.
static int64_t stop_deadline_us;
static int stop_interrupt(JSRuntime *rt, void *opaque) {
    (void)rt; (void)opaque;
    return esp_timer_get_time()>stop_deadline_us;
}

void pocket_app_reset(void) {
    JSContext *ctx=js_ctx;
    // Cleared first, so what it says below is this hook's outcome and not a
    // start hook's from many frames ago.
    hook_failed=false;
    if(ctx && !JS_IsUndefined(stop_hook) &&
       (phase==PHASE_STARTING || phase==PHASE_RUNNING)) {
        JSRuntime *rt=JS_GetRuntime(ctx);
        stop_deadline_us=esp_timer_get_time()+APP_STOP_MS*1000;
        JS_SetInterruptHandler(rt,stop_interrupt,NULL);
        // "back" is the only reason this host can give honestly: the shell has
        // one teardown path, taken both when the user leaves an app and when
        // the app calls exit(), and neither "replace" nor "shutdown" exists
        // here yet. Passing one of those would make the argument a guess.
        JSValue reason=JS_NewString(ctx,"back");
        run_hook(ctx,stop_hook,reason,1);
        JS_FreeValue(ctx,reason);
        // A hook's Promise can only settle through the job queue, and this is
        // past the last turn, so its jobs are drained here. Host I/O is not
        // pumped during the wait: what follows this function is the I/O
        // cancellation of section 5, so a stop hook must not await device work.
        JSContext *pending=NULL;
        while(hook_pending && esp_timer_get_time()<stop_deadline_us)
            if(JS_ExecutePendingJob(rt,&pending)<=0) break;
        if(hook_pending) ESP_LOGW(TAG,"stop hook did not finish in %dms",APP_STOP_MS);
    }
    // The turn that carried an exit() was interrupted on purpose, and the
    // exception it left behind is the only thing the screen that started this
    // run would otherwise have to show. A hook that failed keeps its own
    // message: that one is news, and this one is not.
    if(exit_requested && !hook_failed) jsconsole_set_error("APP EXITED");
    phase=PHASE_STOPPED;
    pocket_api_sub_close_all(&frame_table);
    frame_table.ctx=NULL;
    if(ctx) { JS_FreeValue(ctx,start_hook); JS_FreeValue(ctx,stop_hook); }
    start_hook=stop_hook=JS_UNDEFINED;
    // The sleeps are not settled here: they wait on promise slots, and
    // pocket_api_reset() is what asks them to stop and lets their resolvers go.
    //
    // Last, and after the stop hook: the hook may print, and a ring freed
    // before it ran would simply be taken again. Nothing reads a record once
    // the realm holding read() is gone.
    log_reset();
    js_ctx=NULL;
}

// ------------------------------------------------------------- capabilities

// None of these three names is in section 2's list, which gives examples and
// leaves an unlisted name to answer supported=false. They are declared for the
// same reason pocket_av.c declares power: a program has to be able to detect
// them, and this is where the numbers it would branch on are published. Every
// value below is a limit this file actually enforces.
static const pocket_limit_t app_limits[] = {
    {.name="maxFrameListeners",.kind=POCKET_LIMIT_INT, .number=APP_FRAME_LISTENERS},
    {.name="stopHookMs",       .kind=POCKET_LIMIT_INT, .number=APP_STOP_MS},
    {.name="stopReasons",      .kind=POCKET_LIMIT_TEXT,.text="back"},
    {.name="launchContext",    .kind=POCKET_LIMIT_FLAG,.number=0},  // section 7, later
    {0},
};
static const pocket_limit_t time_limits[] = {
    {.name="maxSleepMs",.kind=POCKET_LIMIT_INT, .number=APP_MAX_TIMEOUT_MS},
    {.name="maxSleeps", .kind=POCKET_LIMIT_INT, .number=APP_SLEEPS},
    {.name="wallClock", .kind=POCKET_LIMIT_FLAG,.number=1},  // once SNTP has run
    {.name="timezone",  .kind=POCKET_LIMIT_FLAG,.number=0},  // reserved, section 8
    {0},
};
static const pocket_limit_t log_limits[] = {
    {.name="maxRecords",    .kind=POCKET_LIMIT_INT,.number=LOG_RECORDS},
    {.name="maxRecordBytes",.kind=POCKET_LIMIT_INT,.number=LOG_RECORD_BYTES},
    {.name="bufferBytes",   .kind=POCKET_LIMIT_INT,.number=LOG_ARENA_BYTES},
    {.name="bytesPerSecond",.kind=POCKET_LIMIT_INT,.number=LOG_BYTES_PER_SEC},
    {.name="maxReadRecords",.kind=POCKET_LIMIT_INT,.number=LOG_READ_MAX},
    {0},
};

static const pocket_capability_t app_capability = {
    .name="app", .supported=true, .available=true, .limits=app_limits,
};
static const pocket_capability_t time_capability = {
    .name="time", .supported=true, .available=true, .limits=time_limits,
};
static const pocket_capability_t log_capability = {
    .name="log", .supported=true, .available=true, .limits=log_limits,
};

// ------------------------------------------------------------------- install

static void define(JSContext *ctx, JSValueConst object, const char *name,
                   JSValue value) {
    JS_DefinePropertyValueStr(ctx,object,name,value,JS_PROP_ENUMERABLE);
}

// The three namespaces this file publishes, plus its one contribution to
// pocket.device. None of the state below moves with them: pocket_app_pump()
// reads `phase` on every turn whether or not the app ever names pocket.app,
// and the console wrapper writes to the log ring from the first line an app
// prints. What is lazy here is only the objects.

static esp_err_t build_app(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    define(ctx,ns,"start",JS_NewCFunction(ctx,js_start,"start",1));
    define(ctx,ns,"exit",JS_NewCFunction(ctx,js_exit,"exit",0));
    define(ctx,ns,"onFrame",JS_NewCFunction(ctx,js_on_frame,"onFrame",1));
    return ESP_OK;
}

static esp_err_t build_time(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    define(ctx,ns,"now",JS_NewCFunction(ctx,js_time_now,"now",0));
    define(ctx,ns,"wall",JS_NewCFunction(ctx,js_time_wall,"wall",0));
    define(ctx,ns,"sleep",JS_NewCFunction(ctx,js_sleep,"sleep",2));
    return ESP_OK;
}

static esp_err_t build_log(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    define(ctx,ns,"write",JS_NewCFunction(ctx,js_log_write,"write",2));
    define(ctx,ns,"read",JS_NewCFunction(ctx,js_log_read,"read",1));
    return ESP_OK;
}

static esp_err_t build_metrics(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    define(ctx,ns,"metrics",JS_NewCFunction(ctx,js_metrics,"metrics",0));
    return ESP_OK;
}

esp_err_t pocket_app_install(JSContext *ctx, void *user_data) {
    (void)user_data;
    pocket_api_register(&app_capability);
    pocket_api_register(&time_capability);
    pocket_api_register(&log_capability);

    // A realm going away takes its callbacks with it, so everything that holds
    // one starts empty on every install.
    for(int i=0;i<APP_FRAME_LISTENERS;i++) {
        frame_slots[i].callback=JS_UNDEFINED;
        frame_slots[i].handle=0;
    }
    frame_table.open=0;
    frame_table.ctx=ctx;
    for(int i=0;i<APP_SLEEPS;i++) sleeps[i].active=false;
    sleeps_open=0;
    phase=PHASE_LOADING;
    registered=false;
    exit_requested=false;
    hook_pending=false; hook_failed=false;
    start_hook=stop_hook=JS_UNDEFINED;
    frame_last_us=0;
    fps_window_us=0; fps_frames=0; fps_value=-1.0;
    log_reset();
    js_ctx=ctx;

    esp_err_t err;
    if((err=pocket_api_lazy(ctx,"app",build_app,NULL))!=ESP_OK) return err;
    if((err=pocket_api_lazy(ctx,"time",build_time,NULL))!=ESP_OK) return err;
    if((err=pocket_api_lazy(ctx,"log",build_log,NULL))!=ESP_OK) return err;
    // device is pocket_api.c's namespace; metrics belongs beside info because
    // section 7 puts it there, and contributing to it is not editing that file.
    // Registering second means info is defined first, as it was when the object
    // was built in one place.
    if((err=pocket_api_lazy(ctx,"device",build_metrics,NULL))!=ESP_OK) return err;

    JSValue global=JS_GetGlobalObject(ctx);
    JSValue console=JS_GetPropertyStr(ctx,global,"console");
    if(JS_IsObject(console)) {
        wrap_console(ctx,console,"log",1);
        wrap_console(ctx,console,"warn",2);
        wrap_console(ctx,console,"error",3);
    }
    JS_FreeValue(ctx,console);
    wrap_console(ctx,global,"print",1);
    JS_FreeValue(ctx,global);
    return ESP_OK;
}
