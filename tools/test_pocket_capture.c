// pocket.audio.capture on a host, with the real QuickJS and the real substrate.
// Host only (WSL: gcc is not on the Windows side):
//
//   wsl -e bash -lc "cd /mnt/c/devs/m5stack/cardputer-adv-pocketjs &&
//                    bash tools/build_pocket_capture_test.sh && /tmp/test-pocket-capture"
//
// What this links is main/pocket/pocket_capture.c and main/pocket/pocket_api.c
// as they are compiled for the board. Only two things are replaced: the
// microphone, which is a fake whose samples are a counting ramp -- so "the
// audio a program reads is time-contiguous" becomes an arithmetic check on the
// numbers rather than a hope -- and the clock, so a timeout can be made to
// happen in a test that takes no time.
//
// The rules being checked are section 9's prose, which is where this API's
// requirements actually live: overflow must fail with LIMIT_EXCEEDED and close
// the recorder rather than splice, a recording must not outlive its session,
// recording and playback are exclusive, and the host must show that the
// microphone is live.
//
// Under -fsanitize=address the buffer a read carries from the pump to the
// Promise is watched too: it changes hands three times (pump, settle, release)
// and a session can end in the middle of that.
#include "pocket_capture.h"
#include "pocket_api.h"
#include "sound.h"
#include "board.h"
#include "esp_app_desc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static unsigned failures;
static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if(!ok) failures++;
}

// ---- the clock ------------------------------------------------------------

static int64_t clock_us=1000000;
int64_t esp_timer_get_time(void) { return clock_us; }

// device.info() reads this out of the substrate; nothing here depends on it.
static const esp_app_desc_t description={.version="host"};
const esp_app_desc_t *esp_app_get_description(void) { return &description; }

// ---- the microphone -------------------------------------------------------
//
// A ramp, because a ramp cannot be spliced quietly: every frame this hands out
// is one more than the frame before it, for the life of the recording, so a
// gap between two reads is a subtraction and not a judgement call.

static bool codec_present=true;   // sound_available()
static bool speaker_busy;         // a tone or clip is playing
static bool mic_open;
static bool mic_overflow;
static int  mic_ready;            // frames waiting in the "ring"
static int  mic_next;             // the next sample value
static int  mic_starts;

bool sound_available(void) { return codec_present; }

bool sound_capture_start(void) {
    if(!codec_present || mic_open || speaker_busy) return false;
    mic_open=true; mic_overflow=false; mic_ready=0; mic_next=1;
    mic_starts++;
    return true;
}

int sound_capture_read(int16_t *out, int max_frames) {
    if(!mic_open || !out) return -2;
    if(mic_overflow) return -1;
    if(max_frames>SOUND_CAPTURE_MAX_FRAMES) max_frames=SOUND_CAPTURE_MAX_FRAMES;
    int n=mic_ready<max_frames?mic_ready:max_frames;
    for(int i=0;i<n;i++) out[i]=(int16_t)(mic_next++);
    mic_ready-=n;
    return n;
}

void sound_capture_stop(void) { mic_open=false; mic_ready=0; }

// The level the indicator draws from. Set directly by the tests: what is being
// checked is what the overlay does with a level, not how sound.c derives one.
static unsigned mic_level;
static bool     mic_clipping;
void sound_capture_level(unsigned *peak, bool *clipping) {
    if(peak) *peak=mic_level;
    if(clipping) *clipping=mic_clipping;
}
bool sound_capture_active(void) { return mic_open; }
uint32_t sound_capture_frames(void) { return (uint32_t)mic_next; }

// ---- the engine ------------------------------------------------------------

static JSRuntime *rt;
static JSContext *ctx;

static char   log_buf[8192];
static size_t log_len;

static JSValue js_note(JSContext *c, JSValueConst self, int argc,
                       JSValueConst *argv) {
    (void)self;
    if(argc>0) {
        const char *s=JS_ToCString(c,argv[0]);
        if(s) {
            int n=snprintf(log_buf+log_len,sizeof log_buf-log_len,"%s;",s);
            if(n>0) log_len+=(size_t)n;
            printf("    js: %s\n",s);
            JS_FreeCString(c,s);
        }
    }
    return JS_UNDEFINED;
}
static bool logged(const char *needle) { return strstr(log_buf,needle)!=NULL; }
static void log_clear(void) { log_len=0; log_buf[0]=0; }

static void drain(void) {
    JSContext *which;
    while(JS_ExecutePendingJob(rt,&which)>0) {}
}

// One turn of the firmware's loop, in the order main/app_session.c runs it.
static void turn(int times) {
    for(int i=0;i<times;i++) {
        pocket_capture_pump();
        pocket_api_pump();
        drain();
    }
}

static void run(const char *source) {
    JSValue value=JS_Eval(ctx,source,strlen(source),"<test>",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(value)) {
        JSValue e=JS_GetException(ctx);
        const char *text=JS_ToCString(ctx,e);
        printf("    threw: %s\n",text?text:"?");
        if(text) JS_FreeCString(ctx,text);
        JS_FreeValue(ctx,e);
        failures++;
    }
    JS_FreeValue(ctx,value);
    drain();
}

// The catch every test uses: the code and the message, in one string the checks
// below can look for.
#define CATCH "function(e){note(WHAT+' '+e.code);}"

static void engine_up(void) {
    rt=JS_NewRuntime();
    ctx=JS_NewContext(rt);
    JSValue global=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,global,"note",JS_NewCFunction(ctx,js_note,"note",1));
    JS_FreeValue(ctx,global);
    if(pocket_api_install(ctx,NULL)!=ESP_OK) { printf("install failed\n"); exit(2); }
    if(pocket_capture_install(ctx,NULL)!=ESP_OK) { printf("capture failed\n"); exit(2); }
}

static void engine_down(void) {
    pocket_capture_reset();
    pocket_api_reset();
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    rt=NULL; ctx=NULL;
}

// ---- the tests -------------------------------------------------------------

// Opens a recorder into globalThis.r and waits for the Promise.
static void open_recorder(const char *spec) {
    char source[256];
    snprintf(source,sizeof source,
        "var WHAT='open';"
        "pocket.audio.capture.open(%s).then(function(x){r=x;note('opened');},"
        CATCH ");",spec);
    run(source);
    turn(2);
}

static void test_capability(void) {
    log_clear();
    run("var c=pocket.capabilities.get('audio.capture');"
        "note('cap '+c.supported+' '+c.available);"
        "note('lim '+c.limits.sampleRate+' '+c.limits.channels+' '+"
        "c.limits.maxReadFrames+' '+c.limits.concurrent+' '+c.limits.fullDuplex);");
    check(logged("cap true true"),"the capability reports supported and available");
    check(logged("lim 24000 1 2048 1 false"),
          "limits publish the rate, the read ceiling and the exclusion");
    codec_present=false;
    log_clear();
    run("var c=pocket.capabilities.get('audio.capture');"
        "note('cap '+c.supported+' '+c.available+' '+c.reason);");
    check(logged("cap true false NO_DEVICE"),"no codec is NO_DEVICE, not NOT_IMPLEMENTED");
    codec_present=true;
}

static void test_open(void) {
    log_clear();
    open_recorder("{sampleRate:24000,channels:1}");
    check(logged("opened"),"open resolves with a recorder");
    check(mic_open,"opening takes the microphone");
    check(sound_capture_active(),"the indicator is live while the recorder is");
    log_clear();
    run("var WHAT='second';pocket.audio.capture.open({}).then(function(){note('second ok');}," CATCH ");");
    turn(2);
    check(logged("second BUSY"),"a second recorder is BUSY, not a second microphone");
    run("r.close();");
    check(!mic_open,"close gives the microphone back");
    check(!sound_capture_active(),"and the indicator goes out with it");

    log_clear();
    run("var WHAT='rate';pocket.audio.capture.open({sampleRate:16000}).then(function(){note('rate ok');}," CATCH ");");
    turn(2);
    check(logged("rate INVALID_ARGUMENT"),
          "a rate this host cannot produce is refused, not silently substituted");
    check(!mic_open,"and refusing it does not leave the microphone open");

    log_clear();
    run("var WHAT='chan';pocket.audio.capture.open({channels:2}).then(function(){note('chan ok');}," CATCH ");");
    turn(2);
    check(logged("chan INVALID_ARGUMENT"),"two channels is refused");

    log_clear();
    speaker_busy=true;
    run("var WHAT='busy';pocket.audio.capture.open({}).then(function(){note('busy ok');}," CATCH ");");
    turn(2);
    speaker_busy=false;
    check(logged("busy BUSY"),"recording while the speaker plays is BUSY: they are exclusive");

    codec_present=false;
    log_clear();
    run("var WHAT='none';pocket.audio.capture.open({}).then(function(){note('none ok');}," CATCH ");");
    turn(2);
    codec_present=true;
    check(logged("none NOT_AVAILABLE"),"no codec is NOT_AVAILABLE");
}

// The heart of it: what read() hands over has to be the audio that came next.
static void test_contiguous(void) {
    log_clear();
    open_recorder("{}");
    mic_ready=100;
    run("var WHAT='read';var first=null,second=null;"
        "r.read(64).then(function(a){first=a;note('a '+a.length+' '+a[0]+' '+a[63]);}," CATCH ");");
    turn(1);
    check(logged("a 64 1 64"),"a read resolves with exactly the frames asked for");
    log_clear();
    run("var WHAT='read2';r.read(36).then(function(a){note('b '+a.length+' '+a[0]+' '+a[35]);}," CATCH ");");
    turn(1);
    check(logged("b 36 65 100"),
          "the next read continues where the last one stopped, with no gap");

    // Nothing buffered: the read waits, and the pump is what finishes it.
    log_clear();
    run("var WHAT='wait';r.read(8).then(function(a){note('c '+a.length+' '+a[0]);}," CATCH ");");
    turn(1);
    check(!logged("c "),"a read with nothing to read does not resolve with an empty array");
    mic_ready=8;
    turn(1);
    check(logged("c 8 101"),"and resolves from the pump when the audio arrives, still in sequence");
    run("r.close();");
}

static void test_read_arguments(void) {
    log_clear();
    open_recorder("{}");
    mic_ready=4000;
    run("var WHAT='big';r.read(2049).then(function(){note('big ok');}," CATCH ");");
    turn(1);
    check(logged("big LIMIT_EXCEEDED"),
          "over maxReadFrames is LIMIT_EXCEEDED, so an app can act on the limit");
    log_clear();
    run("var WHAT='zero';r.read(0).then(function(){note('zero ok');}," CATCH ");");
    turn(1);
    check(logged("zero INVALID_ARGUMENT"),"zero frames is an argument error");
    log_clear();
    run("var WHAT='frac';r.read(1.5).then(function(){note('frac ok');}," CATCH ");");
    turn(1);
    check(logged("frac INVALID_ARGUMENT"),"a fractional frame count is not rounded");
    log_clear();
    // Two at once. The first has nothing to read yet, so it is still in flight.
    mic_ready=0;
    run("var WHAT='two';r.read(16).then(function(){note('two first');}," CATCH ");"
        "r.read(16).then(function(){note('two second');},function(e){note('two '+e.code);});");
    check(logged("two BUSY"),"a second read while one waits is BUSY");
    mic_ready=16;
    turn(1);
    check(logged("two first"),"and the first one still finishes");
    run("r.close();");
}

// Section 9: on overflow, do not splice. Fail with LIMIT_EXCEEDED and close.
static void test_overflow(void) {
    log_clear();
    open_recorder("{}");
    mic_ready=32;
    mic_overflow=true;
    run("var WHAT='ovf';r.read(32).then(function(){note('ovf ok');}," CATCH ");");
    turn(1);
    check(logged("ovf LIMIT_EXCEEDED"),"an overflow is LIMIT_EXCEEDED");
    check(!mic_open,"an overflow closes the recorder rather than splicing over the hole");
    check(!sound_capture_active(),"and the indicator goes out");
    log_clear();
    run("var WHAT='after';r.read(8).then(function(){note('after ok');}," CATCH ");");
    turn(1);
    check(logged("after CLOSED"),"every method of a closed recorder answers CLOSED");
    run("r.close();");   // idempotent, and must not throw
    check(true,"close on a closed recorder is a no-op");

    // The same thing while a read is already waiting: the overflow arrives at
    // the pump, not at the call.
    log_clear();
    open_recorder("{}");
    run("var WHAT='late';r.read(16).then(function(){note('late ok');}," CATCH ");");
    turn(1);
    mic_overflow=true;
    turn(1);
    check(logged("late LIMIT_EXCEEDED"),"an overflow under a waiting read rejects it too");
    check(!mic_open,"and closes the recorder from the pump");
}

static void test_timeout_and_cancel(void) {
    log_clear();
    open_recorder("{}");
    mic_ready=0;
    run("var WHAT='to';r.read(16,{timeoutMs:50}).then(function(){note('to ok');}," CATCH ");");
    turn(1);
    check(!logged("to "),"a read inside its timeout is still waiting");
    clock_us+=60000;
    turn(2);
    check(logged("to TIMEOUT"),"a read that outlives timeoutMs rejects with TIMEOUT");
    check(mic_open,"a timed-out read leaves the recording open");
    log_clear();
    mic_ready=4;
    run("var WHAT='again';r.read(4).then(function(a){note('again '+a[0]);}," CATCH ");");
    turn(1);
    check(logged("again 1"),"and the next read still gets the audio, in sequence");

    log_clear();
    run("var s=pocket.cancel.source();var WHAT='can';"
        "r.read(16,{cancel:s.token}).then(function(){note('can ok');}," CATCH ");"
        "s.cancel();");
    mic_ready=0;
    turn(2);
    check(logged("can CANCELLED"),"a cancelled read rejects with CANCELLED");
    run("r.close();");
}

// Section 9 makes the indicator the host's obligation, so it is drawn from
// board_present() and cannot be painted over by the app that is recording.
static void test_indicator(void) {
    static uint16_t strip[LCD_W*8];
    memset(strip,0,sizeof strip);
    pocket_capture_overlay(strip,0,8);
    bool blank=true;
    for(unsigned i=0;i<sizeof strip/sizeof strip[0];i++) if(strip[i]) blank=false;
    check(blank,"nothing is drawn when nothing is recording");

    log_clear();
    open_recorder("{}");
    pocket_capture_overlay(strip,0,8);
    unsigned painted=0;
    for(unsigned i=0;i<sizeof strip/sizeof strip[0];i++) if(strip[i]) painted++;
    check(painted>=64,"the indicator is drawn while the microphone is live");

    // And it asks for a repaint, at most twice a second, so an app that stopped
    // drawing cannot leave a screen with no dot on it.
    clock_us+=1000000;
    check(pocket_capture_take_dirty(),"a recording asks for a repaint");
    check(!pocket_capture_take_dirty(),"but not twice in the same half second");
    clock_us+=600000;
    check(pocket_capture_take_dirty(),"and again once the half second is up");

    run("r.close();");
    memset(strip,0,sizeof strip);
    pocket_capture_overlay(strip,0,8);
    blank=true;
    for(unsigned i=0;i<sizeof strip/sizeof strip[0];i++) if(strip[i]) blank=false;
    check(blank,"and stops being drawn the moment the recorder closes");
    check(!pocket_capture_take_dirty(),"and stops asking for repaints");
}

// Open, close, open again, within one session. The HAL had a defect exactly
// here -- the second recording of a session delivered half a second and the
// third nothing, because the I2S channel was being torn down and rebuilt around
// each one -- and nothing reached it, because no test and no app had ever opened
// a recorder twice. This covers the surface's half of that: a recorder is
// re-openable and the second one records. The driver's half is not host-visible
// and is checked by the sweep on the board.
static void test_reopen(void) {
    log_clear();
    for(int round=0;round<3;round++) {
        open_recorder("{}");
        mic_ready=8;
        char source[160];
        snprintf(source,sizeof source,
            "var WHAT='r%d';r.read(8).then(function(a){note('r%d '+a.length+' '+a[0]);},"
            CATCH ");",round,round);
        run(source);
        turn(1);
        run("r.close();");
    }
    check(logged("r0 8 1"),"the first recording of a session reads");
    check(logged("r1 8 1"),"so does the second");
    check(logged("r2 8 1"),"and the third");
    check(mic_starts>=3,"each open took the microphone again");
    check(!mic_open,"and the last close gave it back");
}

// close() with a read still waiting: the read has to settle, and CLOSED is what
// it settles with. A Promise handed to an app always settles.
static void test_close_under_read(void) {
    log_clear();
    open_recorder("{}");
    mic_ready=0;
    run("var WHAT='under';r.read(16).then(function(){note('under ok');}," CATCH ");");
    turn(1);
    run("r.close();");
    turn(2);
    check(logged("under CLOSED"),"a read waiting when close() lands rejects with CLOSED");
    check(!mic_open,"and the microphone is back");
}

// The level meter beside the dot. Drawn on the drawing task from a value the
// reading task wrote, and the owner of this board asked for it: while you are
// the one making the sound, the two things you need to know are whether it is
// clipping and whether it is far too quiet.
static unsigned corner_pixels(uint16_t *strip, uint16_t colour) {
    unsigned n=0;
    for(unsigned i=0;i<LCD_W*16;i++) if(strip[i]==colour) n++;
    return n;
}

static void test_level_meter(void) {
    static uint16_t strip[LCD_W*16];
    const uint16_t white=board_rgb(210,222,230), amber=board_rgb(255,176,0);
    log_clear();
    open_recorder("{}");

    // Silence: the dot is there, no cell is lit.
    mic_level=0; mic_clipping=false;
    memset(strip,0,sizeof strip);
    pocket_capture_overlay(strip,0,16);
    check(corner_pixels(strip,board_rgb(232,48,48))>=64,"the dot is drawn regardless of level");
    check(corner_pixels(strip,white)==0,"no cell is lit when nothing is being heard");

    // A quiet signal lights fewer cells than a loud one, and the scale is in
    // doublings, so each factor of two is one more cell.
    mic_level=600;                        // ~1/54 of full scale
    memset(strip,0,sizeof strip);
    pocket_capture_overlay(strip,0,16);
    unsigned quiet=corner_pixels(strip,white);
    mic_level=20000;
    memset(strip,0,sizeof strip);
    pocket_capture_overlay(strip,0,16);
    unsigned loud=corner_pixels(strip,white);
    check(quiet>0,"a quiet signal lights something");
    check(loud>quiet,"a louder signal lights more of the meter");

    // Clipping is the only thing that changes colour, so a colour change in
    // this corner always means something is wrong.
    mic_level=32767; mic_clipping=true;
    memset(strip,0,sizeof strip);
    pocket_capture_overlay(strip,0,16);
    check(corner_pixels(strip,amber)>0,"clipping shows amber");
    check(corner_pixels(strip,board_rgb(232,48,48))>=64,"and the dot keeps its own colour");
    mic_clipping=false;
    memset(strip,0,sizeof strip);
    pocket_capture_overlay(strip,0,16);
    check(corner_pixels(strip,amber)==0,"and no amber when it is merely loud");

    // And none of it exists when no recording does.
    run("r.close();");
    memset(strip,0,sizeof strip);
    pocket_capture_overlay(strip,0,16);
    check(corner_pixels(strip,white)==0 && corner_pixels(strip,amber)==0,
          "the meter goes with the dot when the recorder closes");
    mic_level=0;
}

// A recording is valid only during an authorised session. The session ending is
// what has to take the microphone back, because nothing else will.
static void test_session_end(void) {
    log_clear();
    open_recorder("{}");
    mic_ready=0;
    run("var WHAT='end';r.read(16).then(function(){note('end ok');}," CATCH ");");
    turn(1);
    check(mic_open,"the microphone is open with a read in flight");
    // app_stop()'s order: the surface first, then the substrate.
    pocket_capture_reset();
    pocket_api_reset();
    check(!mic_open,"a session ending closes the recording");
    check(!sound_capture_active(),"and the indicator with it");
    // The realm is still up here; what matters is that nothing was left holding
    // the buffer or the slot, which ASan and the runtime free below decide.
}

int main(void) {
    printf("pocket.audio.capture, host, real QuickJS\n");
    engine_up();
    test_capability();
    test_open();
    test_contiguous();
    test_read_arguments();
    test_overflow();
    test_timeout_and_cancel();
    test_indicator();
    test_reopen();
    test_close_under_read();
    test_level_meter();
    test_session_end();
    engine_down();

    // A second session on a fresh realm: the surface's state is static, so a
    // recorder left behind by the first would be visible to the second.
    engine_up();
    log_clear();
    open_recorder("{}");
    check(logged("opened"),"a second session opens a recorder of its own");
    check(mic_starts>=2,"and takes the microphone again");
    run("r.close();");
    engine_down();

    printf("%s: %u failure(s)\n",failures?"FAILED":"passed",failures);
    return failures?1:0;
}
