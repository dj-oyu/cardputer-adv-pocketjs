#include "opus_feed.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "opus.h"
#include <string.h>

// The decode task. See opus_feed.h for the container and for why the gate
// refuses everything but CELT 20 ms mono.
//
// THE STACK, which is the number this whole arrangement exists to respect.
// 10,420 bytes is MEASURED (2026-09-07, the ble-wt harness, docs/opus-
// feasibility.md §2) and it is measured for CELT WB 20 ms mono and nothing else.
// VAR_ARRAYS=1 puts CELT's scratch on this stack rather than in a static
// pseudo-stack, which is why libopus costs zero static DIRAM here and why the
// number lands in this file instead of in the map.
//
// It was 14,336 (10,420 plus 3,916) until a board run said what actually binds
// here, and it is not free heap. MEASURED 2026-09-09 with a linked radio:
//
//   linked            free 56,204   largest 31,744
//   play() refused    free 41,872   largest 31,744
//
// Free was never the problem -- 56,204 against the 45,060 playback wants. The
// largest run was. It stayed at 31,744 across the refusal, so the two 6,144
// rings did not come out of it, and everything outside it totalled 10,128,
// which holds neither of the two big blocks. So the decoder's 18,436 and this
// stack must BOTH come out of that one run, and 18,436 + 14,336 = 32,772
// against 31,744 -- short by about a kilobyte, before per-block overhead.
//
// 12,288 is what makes them fit: 30,724 out of 31,744, with about a kilobyte
// left. The cost is margin. The high-water mark this task prints has been
// 10,472 to 10,476 across builds (measured), so 12,288 keeps 1,812 bytes --
// 17%, down from 37%. That is defensible ONLY because the packet gate in
// opus_feed.h makes CELT 20 ms mono the only code path that can run here, which
// is the path those 10,476 were measured on. It would not be defensible if this
// host decoded SILK or hybrid, and it is the reason the gate is not negotiable.
//
// The task reports its own high-water at the end of every stream. If a build
// ever prints stack_used above about 11,300, this constant has to move back up
// and something else has to give -- and the print is what says so before an
// overflow does.
#define DEC_STACK   12288
#define DEC_PRIO    6

// How many packets go into one PCM slot. A slot is 2,048 bytes = 1,024 frames;
// a packet is 480. Two fit with 64 frames to spare, three do not. So a slot is
// 960 frames = 40 ms and the three-slot ring holds 120 ms of decoded audio,
// against a decode that costs 3.2 ms per 20 ms. That is the margin the ui task's
// 39.9 ms frames get spent out of.
#define PACKETS_PER_SLOT 2

// The one pool everything here competes for: internal 8-bit DRAM. Named so the
// two refusals below and the numbers they print cannot drift apart.
#define HEAP_POOL (MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)

static TaskHandle_t     dec_task;
static sound_stream_t  *dec_pcm, *dec_packets;
static uint32_t         dec_frames;      // output frames the stream owes
static uint16_t         dec_skip;
// Written on one task and read on the other, so atomics rather than volatile:
// `running` is the whole of the handshake that lets opus_feed_stop() promise the
// rings are no longer touched, the same promise sound_stream_stop() makes.
static atomic_bool      dec_halt, dec_running;
static atomic_uint      dec_faults;
// How long the task took to publish its FIRST slot: created, decoder built,
// two packets decoded. Nothing can be heard before this elapses, so it is the
// length of the only window in which this design can starve without something
// far more visible being wrong -- see the priming handshake in pocket_av.c.
static atomic_uint      dec_prime_us;
// Built by opus_feed_start() on the CALLER's task, not by the task itself, and
// this is a correctness fix rather than a tidy-up. It used to be created inside
// dec_task_fn, which put the single largest allocation this feature makes --
// 18,436 bytes, one block -- on the far side of a boundary the error path does
// not cross: opus_feed_start() had already returned true, so the only way to
// report a failure was to set eof and raise a fault, which reaches the app as
// P_ERROR and therefore as IO_ERROR. An out-of-memory wore the code for a bad
// read, and cost a board run that could not answer the question it was flashed
// to answer (2026-09-09, with a linked radio; the app was told IO_ERROR about
// one frame later, which is the tell -- a real read failure or a prime timeout
// takes about a second).
//
// This is the same shape as the priming bug in docs/common-api.md 9.1.2. There
// the WORK did not cross the task boundary; here the ERROR did not. A task
// cannot be primed by calling a function, and it cannot report a failure it has
// not yet had time to have.
//
// The order is deliberate too: the decoder's 18,436 is taken BEFORE the task's
// 14,336, so the larger block gets first refusal on the largest free run. The
// two together are 32,772 out of one pool and both are single blocks; which one
// fails first changes only which number the app is shown.
static OpusDecoder     *dec_handle;

// One packet out of the compressed ring, gated and decoded straight into `out`.
// Returns frames produced, 0 for a refusal (dec_faults says which).
static int decode_one(OpusDecoder *dec, const uint8_t *pkt, uint32_t len,
                      int16_t *out) {
    if(!opus_pak_toc_ok(pkt[0])) {
        // Named rather than counted: the app gets "this host decodes CELT"
        // through onState, and whoever encoded the asset gets the TOC byte here.
        ESP_LOGE("opus","packet is not CELT 20 ms mono (toc=0x%02x)",pkt[0]);
        atomic_fetch_add(&dec_faults,1);
        return 0;
    }
    int n=opus_decode(dec,pkt,(opus_int32)len,out,(int)OPUS_PAK_FRAME_SAMPLES,0);
    if(n!=(int)OPUS_PAK_FRAME_SAMPLES) {
        ESP_LOGE("opus","opus_decode returned %d for a %u-byte packet",n,(unsigned)len);
        atomic_fetch_add(&dec_faults,1);
        return 0;
    }
    return n;
}

static void dec_task_fn(void *arg) {
    (void)arg;
    // Built and checked by opus_feed_start(); the task owns it from here and
    // destroys it at the bottom, on every exit.
    OpusDecoder *dec=dec_handle;

    stream_read_t r={0};
    uint32_t produced=0, skip=dec_skip, decoded=0;
    int64_t total_us=0, worst_us=0, born=esp_timer_get_time();
    bool ended=false, primed=false;

    while(!atomic_load(&dec_halt)&&!ended) {
        uint8_t *slot=sound_stream_slot(dec_pcm);
        if(!slot) { vTaskDelay(1); continue; }  // the audio task is behind
        uint32_t bytes=0;
        for(int p=0;p<PACKETS_PER_SLOT;p++) {
            // A packet at a time out of the compressed ring. block=0 because
            // this ring carries no ADPCM and wants no predictor.
            while(!r.held||r.used>=r.bytes) {
                stream_release(dec_packets,&r);
                if(!stream_take(dec_packets,&r,0)) {
                    if(atomic_load(&dec_packets->eof)) ended=true;
                    break;
                }
            }
            if(ended) break;
            if(!r.held||r.used+2>r.bytes) { vTaskDelay(1); p--; continue; }
            uint32_t len=(uint32_t)r.at[r.used]|((uint32_t)r.at[r.used+1]<<8);
            if(!len||r.used+2+len>r.bytes) {   // opus_pak_whole guarantees neither
                ESP_LOGE("opus","a slot ended inside a packet at %u of %u",
                         (unsigned)r.used,(unsigned)r.bytes);
                atomic_fetch_add(&dec_faults,1); ended=true; break;
            }
            const uint8_t *pkt=r.at+r.used+2;
            r.used+=2+len;
            int64_t t0=esp_timer_get_time();
            int n=decode_one(dec,pkt,len,(int16_t *)(slot+bytes));
            int64_t dt=esp_timer_get_time()-t0;
            if(!n) { ended=true; break; }
            total_us+=dt; if(dt>worst_us) worst_us=dt; decoded++;
            // The encoder's lookahead, discarded from the head of the stream.
            // totalFrames in the header already excludes it, so dropping it here
            // is what makes the two agree.
            if(skip) {
                uint32_t drop=skip<(uint32_t)n?skip:(uint32_t)n;
                memmove(slot+bytes,slot+bytes+drop*2,((uint32_t)n-drop)*2);
                n-=(int)drop; skip-=drop;
            }
            bytes+=(uint32_t)n*2;
            produced+=(uint32_t)n;
            if(produced>=dec_frames) { ended=true; break; }
        }
        // A short slot is legal for PCM16 -- stream_sample walks `length`, not
        // the slot size -- so the tail publishes whatever it has rather than
        // padding with silence that would land inside the audio.
        if(bytes||ended) {
            sound_stream_publish(dec_pcm,bytes,ended);
            if(!primed) {
                primed=true;
                atomic_store(&dec_prime_us,
                             (unsigned)(esp_timer_get_time()-born));
            }
        }
    }
    if(atomic_load(&dec_halt)&&!ended) atomic_store(&dec_pcm->eof,true);
    stream_release(dec_packets,&r);

    unsigned left=uxTaskGetStackHighWaterMark(NULL)*sizeof(StackType_t);
    // The shipped decoder reporting its own cost, rather than inheriting the
    // harness's. mean/worst are what the frame budget is argued from and
    // stack_used is what DEC_STACK is argued from; both belong to this build.
    ESP_LOGI("opus","OPUSDEC packets=%u frames=%u faults=%u mean_us=%d worst_us=%d "
                    "prime_us=%u stack_used=%u of %d",
             (unsigned)decoded,(unsigned)produced,(unsigned)atomic_load(&dec_faults),
             (int)(decoded?total_us/(int64_t)decoded:0),(int)worst_us,
             (unsigned)atomic_load(&dec_prime_us),DEC_STACK-left,DEC_STACK);
    opus_decoder_destroy(dec);
    dec_handle=NULL;
    atomic_store(&dec_running,false);
    vTaskDelete(NULL);
}

opus_feed_start_t opus_feed_start(sound_stream_t *pcm, sound_stream_t *packets,
                                  uint32_t frames, uint16_t skip) {
    if(atomic_load(&dec_running)) return OPUS_FEED_BUSY;
    dec_pcm=pcm; dec_packets=packets; dec_frames=frames; dec_skip=skip;
    atomic_store(&dec_halt,false); atomic_store(&dec_faults,0);
    atomic_store(&dec_prime_us,0);

    // Both refusals below carry the heap they were refused at. "There is no
    // room for the stream" without a number is the sentence that sent somebody
    // looking at the filesystem, and the two numbers are not interchangeable:
    // this feature fails on the LARGEST BLOCK long before it fails on free.
    int err=OPUS_OK;
    dec_handle=opus_decoder_create(24000,1,&err);
    if(!dec_handle||err!=OPUS_OK) {
        ESP_LOGE("opus","opus_decoder_create failed (%d): %u free, %u largest",
                 err,(unsigned)heap_caps_get_free_size(HEAP_POOL),
                 (unsigned)heap_caps_get_largest_free_block(HEAP_POOL));
        dec_handle=NULL;
        return OPUS_FEED_NOMEM;
    }

    atomic_store(&dec_running,true);
    if(xTaskCreate(dec_task_fn,"opusdec",DEC_STACK/sizeof(StackType_t),NULL,
                   DEC_PRIO,&dec_task)!=pdPASS) {
        ESP_LOGE("opus","the decode task (%d of stack) would not start: "
                        "%u free, %u largest",DEC_STACK,
                 (unsigned)heap_caps_get_free_size(HEAP_POOL),
                 (unsigned)heap_caps_get_largest_free_block(HEAP_POOL));
        atomic_store(&dec_running,false);
        opus_decoder_destroy(dec_handle);
        dec_handle=NULL;
        return OPUS_FEED_NOMEM;
    }
    return OPUS_FEED_OK;
}

bool opus_feed_stop(void) {
    if(!atomic_load(&dec_running)) return true;
    atomic_store(&dec_halt,true);
    // The task's longest uninterruptible step is one packet (worst 3.7 ms
    // measured) plus a tick of backoff, so 200 ms is far past any honest wait.
    // Returning false means it still holds both rings, which is not a thing to
    // recover from by freeing them anyway -- the same handshake, and the same
    // reasoning, as sound_stream_stop().
    for(int i=0;i<40&&atomic_load(&dec_running);i++) vTaskDelay(pdMS_TO_TICKS(5));
    return !atomic_load(&dec_running);
}

uint32_t opus_feed_faults(void) { return atomic_load(&dec_faults); }
