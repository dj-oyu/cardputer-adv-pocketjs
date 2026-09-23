#include "mp3_sd_session.h"
#include "pocket_fs.h"
#include "mp3_feed.h"
#include "mp3_sd_reader.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdlib.h>

#define SD_STACK 4096
#define SD_WAIT_MS 5
#define SD_ACK_WAIT_MS 200
#define SD_RING_BYTES (SOUND_STREAM_SLOTS*SOUND_STREAM_SLOT_BYTES)

typedef enum { SD_READING, SD_EOF, SD_ERROR, SD_CANCELLED } sd_terminal_t;
struct mp3_sd_session {
    sd_media_read_lease_t lease;
    sound_stream_t pcm, packets;
    uint8_t *pcm_bytes, *packet_bytes;
    uint32_t first, end;
    atomic_uint position;
    atomic_int terminal;
    atomic_bool paused, cancel;
    atomic_bool started;
    TaskHandle_t worker;
    portMUX_TYPE worker_lock;
    struct mp3_sd_session *next;
    bool audio_stopped, decoder_stopped;
#ifdef KASANE_P0_PROBE
    int64_t stop_started, ack_at;
    uint32_t ring_low_water, core, stack_free;
#endif
};
static mp3_sd_session_t *quarantine;

static void wake(mp3_sd_session_t *s) {
    taskENTER_CRITICAL(&s->worker_lock);
    if(s->worker) xTaskNotifyGive(s->worker);
    taskEXIT_CRITICAL(&s->worker_lock);
}

static int32_t lease_read(void *ctx, uint32_t at, uint8_t *slot,
                          uint32_t want, int *err) {
    return sd_media_read_lease_read_at(ctx,at,slot,want,err);
}

static void sd_worker(void *arg) {
    mp3_sd_session_t *s=arg;
    while(!atomic_load(&s->started)) vTaskDelay(1);
    mp3_sd_reader_t r={.packets=&s->packets,.at=s->first,.end=s->end,
                       .cancel=&s->cancel,.revoked=&s->lease.token.revoked,
                       .read_ctx=&s->lease,.read=lease_read};
#ifdef KASANE_P0_PROBE
    s->core=(uint32_t)xPortGetCoreID();
#endif
    for(;;) {
        if(atomic_load(&s->cancel)||atomic_load(&s->lease.token.revoked)) {
            atomic_store(&s->terminal,SD_CANCELLED); break;
        }
        if(atomic_load(&s->paused)) {
            ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(SD_WAIT_MS));
            continue;
        }
#ifdef KASANE_P0_PROBE
        uint32_t buffered=(uint32_t)(atomic_load(&s->packets.filled)-
                                      atomic_load(&s->packets.drained));
        if(r.at!=s->first&&buffered<s->ring_low_water)
            s->ring_low_water=buffered;
#endif
        mp3_sd_step_t step=mp3_sd_reader_step(&r);
        atomic_store(&s->position,r.at);
        if(step==MP3_SD_CANCEL) { atomic_store(&s->terminal,SD_CANCELLED); break; }
        if(step==MP3_SD_ERROR) { atomic_store(&s->terminal,SD_ERROR); break; }
        if(step==MP3_SD_EOF) { atomic_store(&s->terminal,SD_EOF); break; }
        if(step==MP3_SD_WAIT)
            ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(SD_WAIT_MS));
    }
#ifdef KASANE_P0_PROBE
    s->stack_free=(uint32_t)uxTaskGetStackHighWaterMark(NULL);
#endif
    taskENTER_CRITICAL(&s->worker_lock);
    s->worker=NULL;
    taskEXIT_CRITICAL(&s->worker_lock);
#ifdef KASANE_P0_PROBE
    s->ack_at=esp_timer_get_time();
#endif
    sd_media_read_lease_ack(&s->lease);
    vTaskDelete(NULL);
}

static void dispose(mp3_sd_session_t *s) {
    if(!sd_media_read_lease_close(&s->lease)) return;
#ifdef KASANE_P0_PROBE
    ESP_LOGI("KSN_P0","SD session=mp3 mode=%s core=%lu opens=%lu reads=%lu bytes=%lu max_us=%lu slow=%lu low_water=%lu stop_ack_us=%lu stack_free=%lu heap_free=%u heap_min=%u heap_largest=%u",
             s->lease.mode==SD_MEDIA_LEASE_REOPEN?"reopen":"persistent",
             (unsigned long)s->core,(unsigned long)s->lease.open_count,
             (unsigned long)s->lease.read_count,(unsigned long)s->lease.read_bytes,
             (unsigned long)s->lease.read_max_us,(unsigned long)s->lease.read_slow_count,
             (unsigned long)s->ring_low_water,
             (unsigned long)(s->ack_at>s->stop_started?
                             s->ack_at-s->stop_started:0),
             (unsigned long)s->stack_free,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
#endif
    free(s->pcm_bytes); free(s->packet_bytes); free(s);
}

mp3_sd_session_t *mp3_sd_session_start(const char *path, uint32_t first,
                                        uint32_t end, const char **code) {
    mp3_sd_session_reap();
    mp3_sd_session_t *s=calloc(1,sizeof(*s));
    if(!s) { if(code) *code="nomem"; return NULL; }
    s->worker_lock=(portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    s->first=first; s->end=end;
    atomic_init(&s->position,first);
    atomic_init(&s->terminal,SD_READING);
    atomic_init(&s->paused,false);
    atomic_init(&s->cancel,false);
    atomic_init(&s->started,false);
    s->pcm_bytes=malloc(SD_RING_BYTES);
    s->packet_bytes=malloc(SD_RING_BYTES);
    if(!s->pcm_bytes||!s->packet_bytes) {
        free(s->pcm_bytes); free(s->packet_bytes); free(s);
        if(code) *code="nomem";
        return NULL;
    }
    s->pcm.bytes=s->pcm_bytes; s->packets.bytes=s->packet_bytes;
    sound_stream_rewind(&s->pcm); sound_stream_rewind(&s->packets);
    uint32_t size=0;
    if(!pocket_fs_sd_read_lease_open(path,&s->lease,SD_MEDIA_LEASE_REOPEN,&size,code)) {
        free(s->pcm_bytes); free(s->packet_bytes); free(s); return NULL;
    }
    if(end>size||first>=end) {
        sd_media_read_lease_ack(&s->lease);
        sd_media_read_lease_close(&s->lease);
        free(s->pcm_bytes); free(s->packet_bytes); free(s);
        if(code) *code="unavailable";
        return NULL;
    }
#ifdef KASANE_P0_PROBE
    s->ring_low_water=SOUND_STREAM_SLOTS;
#endif
    // Fixed to CPU0, below the decoder (6) and audio task priorities.
    if(xTaskCreatePinnedToCore(sd_worker,"mp3sd",SD_STACK,s,4,&s->worker,0)!=pdPASS) {
        sd_media_read_lease_ack(&s->lease);
        sd_media_read_lease_close(&s->lease);
        free(s->pcm_bytes); free(s->packet_bytes); free(s);
        if(code) *code="nomem";
        return NULL;
    }
    atomic_store(&s->started,true);
    return s;
}

sound_stream_t *mp3_sd_session_pcm(mp3_sd_session_t *s) { return &s->pcm; }
sound_stream_t *mp3_sd_session_packets(mp3_sd_session_t *s) { return &s->packets; }
void mp3_sd_session_pause(mp3_sd_session_t *s, bool paused) {
    if(!s) return;
    atomic_store(&s->paused,paused);
    wake(s);
}
bool mp3_sd_session_fault(const mp3_sd_session_t *s) {
    return s&&atomic_load(&s->terminal)==SD_ERROR;
}
uint32_t mp3_sd_session_position(const mp3_sd_session_t *s) {
    return s?atomic_load(&s->position):0;
}
bool mp3_sd_session_stop(mp3_sd_session_t *s, bool audio_stopped,
                         bool decoder_stopped) {
    if(!s) return true;
#ifdef KASANE_P0_PROBE
    s->stop_started=esp_timer_get_time();
#endif
    s->audio_stopped=audio_stopped;
    s->decoder_stopped=decoder_stopped;
    atomic_store(&s->cancel,true);
    atomic_store(&s->paused,false);
    sd_media_read_lease_cancel(&s->lease);
    wake(s);
    for(unsigned i=0;i<SD_ACK_WAIT_MS/SD_WAIT_MS&&
                      !sd_media_read_lease_acked(&s->lease);i++)
        vTaskDelay(pdMS_TO_TICKS(SD_WAIT_MS));
    if(sd_media_read_lease_acked(&s->lease)&&audio_stopped&&decoder_stopped) {
        dispose(s); return true;
    }
    s->next=quarantine; quarantine=s;
    ESP_LOGW("mp3sd","session quarantined: ACK=%u audio=%u decoder=%u",
             (unsigned)sd_media_read_lease_acked(&s->lease),
             (unsigned)audio_stopped,(unsigned)decoder_stopped);
    return false;
}
void mp3_sd_session_reap(void) {
    for(mp3_sd_session_t **at=&quarantine;*at;) {
        mp3_sd_session_t *s=*at;
        if(sd_media_read_lease_acked(&s->lease)&&s->audio_stopped&&
           (s->decoder_stopped||!mp3_feed_running())) {
            *at=s->next;
            dispose(s);
        } else at=&s->next;
    }
}
