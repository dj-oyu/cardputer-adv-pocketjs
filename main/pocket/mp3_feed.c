#include "mp3_feed.h"
#include "mp3_decode.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <string.h>

// Playback-only allocation. Measure this worker, not a desktop decoder stack.
#define MP3_STACK 24576
typedef struct {
    pocket_mp3_decoder_t decoder;
    uint8_t *frame;
    sound_stream_t *pcm, *input;
    stream_read_t read;
    uint8_t *slot;
    unsigned used;
    uint32_t skip, produced;
    int64_t blocked_us;
} mp3_worker_t;
static atomic_bool running, halt, parked, worker_started;
static atomic_uint faults, frames, progress;
static TaskHandle_t worker_task;
static portMUX_TYPE worker_lock=portMUX_INITIALIZER_UNLOCKED;

static void wake_worker(void) {
    taskENTER_CRITICAL(&worker_lock);
    if(worker_task) xTaskNotifyGive(worker_task);
    taskEXIT_CRITICAL(&worker_lock);
}

static void wait_while_paused(void) {
    while(atomic_load(&parked)&&!atomic_load(&halt))
        ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(5));
}

static int read_bytes(mp3_worker_t *w, uint8_t *dst, unsigned want) {
    unsigned got=0;
    while(got<want&&!atomic_load(&halt)) {
        wait_while_paused();
        if(atomic_load(&halt)) break;
        if(!w->read.held||w->read.used==w->read.bytes) {
            stream_release(w->input,&w->read);
            if(!stream_take(w->input,&w->read,0)) {
                if(atomic_load(&w->input->eof)) {
                    // MP3 publishes the last slot before eof. Acquire eof,
                    // then recheck filled to close the concurrent-tail race.
                    if(!stream_take(w->input,&w->read,0)) break;
                } else { vTaskDelay(1); continue; }
            }
        }
        unsigned n=w->read.bytes-w->read.used;
        if(n>want-got) n=want-got;
        memcpy(dst+got,w->read.at+w->read.used,n);
        w->read.used+=n; got+=n;
    }
    return (int)got;
}

static bool output(void *ctx, int16_t sample) {
    mp3_worker_t *w=ctx;
    w->produced++;
    if(w->skip) { w->skip--; return !atomic_load(&halt); }
    while(!w->slot&&!atomic_load(&halt)) {
        wait_while_paused();
        w->slot=sound_stream_slot(w->pcm);
        if(!w->slot) {
            int64_t start=esp_timer_get_time();
            vTaskDelay(1);
            w->blocked_us+=esp_timer_get_time()-start;
        }
    }
    if(atomic_load(&halt)) return false;
    memcpy(w->slot+w->used,&sample,2);
    w->used+=2;
    if(w->used==SOUND_STREAM_SLOT_BYTES) {
        sound_stream_publish(w->pcm,w->used,false);
        w->slot=NULL; w->used=0;
    }
    return true;
}

static void worker(void *arg) {
    mp3_worker_t *w=arg;
    while(!atomic_load(&worker_started)) vTaskDelay(1);
    int64_t total_us=0, worst_us=0;
    uint32_t count=0;
    while(!atomic_load(&halt)) {
        wait_while_paused();
        if(atomic_load(&halt)) break;
        int n=read_bytes(w,w->frame,4);
        if(!n) break;
        pocket_mp3_header_t h;
        if(n!=4||!pocket_mp3_header(w->frame,&h)||
           read_bytes(w,w->frame+4,h.bytes-4)!=(int)h.bytes-4) {
            if(!atomic_load(&halt)) atomic_fetch_add(&faults,1);
            break;
        }
        int64_t start=esp_timer_get_time();
        w->blocked_us=0;
        bool ok=pocket_mp3_decode(&w->decoder,w->frame,h.bytes,output,w);
        int64_t dt=esp_timer_get_time()-start-w->blocked_us;
        if(!ok) {
            if(!atomic_load(&halt)) atomic_fetch_add(&faults,1);
            break;
        }
        total_us+=dt; if(dt>worst_us) worst_us=dt;
        count++;
        atomic_store(&progress,count);
        atomic_store(&frames,w->produced);
        // Yield even when no PCM backpressure applies.
        // A live pause retains this decoder instead of replaying the prefix.
        vTaskDelay(1);
    }
    if(!count&&!atomic_load(&halt)) atomic_fetch_add(&faults,1);
    if(!atomic_load(&halt)) {
        while(!w->slot&&!atomic_load(&halt)) {
            w->slot=sound_stream_slot(w->pcm);
            if(!w->slot) vTaskDelay(1);
        }
        if(w->slot) {
            sound_stream_publish(w->pcm,w->used,false);
            atomic_store(&w->pcm->eof,true);
        }
    }
    stream_release(w->input,&w->read);
    unsigned left=uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI("mp3","MP3DEC packets=%u frames=%u faults=%u mean_us=%u "
                   "worst_us=%u stack_used=%u of %u state_bytes=%u",
             (unsigned)count,(unsigned)w->produced,(unsigned)atomic_load(&faults),
             (unsigned)(count?total_us/count:0),(unsigned)worst_us,
             MP3_STACK-left,MP3_STACK,(unsigned)sizeof(*w));
#ifdef KASANE_P0_BUS_PROBE
    ESP_LOGI("mp3","P1 decoder observed core %d",xPortGetCoreID());
#endif
    free(w->decoder.pcm); free(w->frame); free(w);
    taskENTER_CRITICAL(&worker_lock);
    worker_task=NULL;
    taskEXIT_CRITICAL(&worker_lock);
    atomic_store(&running,false);
    vTaskDelete(NULL);
}

mp3_feed_start_t mp3_feed_start(sound_stream_t *pcm, sound_stream_t *input,
                                uint32_t skip_frames) {
    if(atomic_load(&running)) return MP3_FEED_BUSY;
    mp3_worker_t *w=calloc(1,sizeof(*w));
    if(!w) return MP3_FEED_NOMEM;
    // Split small buffers from the state so the decoder and stack do not
    // demand two allocations larger than the free blocks left by Wi-Fi.
    int16_t *pcm_buffer=malloc(MINIMP3_MAX_SAMPLES_PER_FRAME*sizeof(int16_t));
    w->frame=malloc(2048);
    if(!pcm_buffer||!w->frame) {
        free(pcm_buffer); free(w->frame); free(w); return MP3_FEED_NOMEM;
    }
    pocket_mp3_init(&w->decoder,pcm_buffer);
    w->pcm=pcm; w->input=input; w->skip=skip_frames;
    atomic_store(&halt,false); atomic_store(&parked,false);
    atomic_store(&worker_started,false);
    atomic_store(&faults,0);
    atomic_store(&frames,0); atomic_store(&progress,0);
    atomic_store(&running,true);
    // ESP-IDF stack sizes and watermarks are bytes, not vanilla FreeRTOS words.
    TaskHandle_t created=NULL;
    /* Decoder bursts on the UI core delayed the LCD completion wake by up to
     * 7 ms during SD playback. Core 0 keeps the UI's core 1 runnable; the
     * reader and output tasks already coexisted there without underruns in
     * the 240-second Cardputer gate (docs/kasane/decisions.md and verification.md). */
    BaseType_t started=xTaskCreatePinnedToCore(worker,"mp3dec",MP3_STACK,w,6,&created,0);
    if(started!=pdPASS) {
        atomic_store(&running,false);
        free(w->decoder.pcm); free(w->frame); free(w); return MP3_FEED_NOMEM;
    }
    taskENTER_CRITICAL(&worker_lock);
    worker_task=created;
    taskEXIT_CRITICAL(&worker_lock);
    atomic_store(&worker_started,true);
    return MP3_FEED_OK;
}

bool mp3_feed_stop(void) {
    atomic_store(&halt,true);
    wake_worker();
    for(unsigned i=0;i<40&&atomic_load(&running);i++) vTaskDelay(pdMS_TO_TICKS(5));
    return !atomic_load(&running);
}
void mp3_feed_pause(bool paused) {
    atomic_store(&parked,paused);
    wake_worker();
}
bool mp3_feed_running(void) { return atomic_load(&running); }
uint32_t mp3_feed_faults(void) { return atomic_load(&faults); }
uint32_t mp3_feed_frames(void) { return atomic_load(&frames); }
uint32_t mp3_feed_progress(void) { return atomic_load(&progress); }
