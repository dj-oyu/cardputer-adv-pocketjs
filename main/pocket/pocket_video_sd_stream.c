#include "pocket_video_sd_stream.h"
#include "pocket_video_stream.h"
#include "pocket_fs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdatomic.h>
#include <stdlib.h>

#define WAIT_MS 5
#define STOP_MS 200
enum { SLOT_EMPTY, SLOT_WRITING, SLOT_READY, SLOT_BORROWED };
struct pocket_video_sd_stream {
    sd_media_read_lease_t lease;
    pocket_video_stream parser;
    uint8_t *pixels;
    uint32_t bytes;
    uint64_t pts, clock_us;
    uint16_t width,height;
    portMUX_TYPE lock;
    atomic_bool cancel,paused,started;
    atomic_int terminal;
    int slot;
    TaskHandle_t worker;
    struct pocket_video_sd_stream *next;
};
static pocket_video_sd_stream *quarantine;

static int32_t lease_read(void *ctx,uint32_t at,uint8_t *out,uint32_t want){
    int error=0;
    return sd_media_read_lease_read_at(ctx,at,out,want,&error);
}
static void worker(void *arg){
    pocket_video_sd_stream *s=arg;
    while(!atomic_load(&s->started))vTaskDelay(1);
    if(!pocket_video_stream_open(&s->parser,lease_read,&s->lease,s->lease.size)||
       s->parser.width!=s->width||s->parser.height!=s->height){
        atomic_store(&s->terminal,-1);
    }else for(;;){
        if(atomic_load(&s->cancel))break;
        if(atomic_load(&s->lease.token.revoked)){
            atomic_store(&s->terminal,-1);
            break;
        }
        if(atomic_load(&s->paused)){
            vTaskDelay(pdMS_TO_TICKS(WAIT_MS));continue;
        }
        taskENTER_CRITICAL(&s->lock);
        uint64_t clock=s->clock_us;
        bool borrowed=s->slot==SLOT_BORROWED;
        bool had_ready=s->slot==SLOT_READY;
        if(!borrowed)s->slot=SLOT_WRITING;
        taskEXIT_CRITICAL(&s->lock);
        if(borrowed){vTaskDelay(pdMS_TO_TICKS(WAIT_MS));continue;}
        uint64_t pts=0;
        pocket_video_stream_result result=pocket_video_stream_latest(
            &s->parser,clock,s->pixels,s->bytes,&pts,8);
        if(result==POCKET_VIDEO_STREAM_FRAME){
            taskENTER_CRITICAL(&s->lock);
            s->pts=pts;s->slot=SLOT_READY;
            taskEXIT_CRITICAL(&s->lock);
            continue; /* A later due record may replace this staging frame. */
        }
        taskENTER_CRITICAL(&s->lock);
        if(result==POCKET_VIDEO_STREAM_INVALID||
           result==POCKET_VIDEO_STREAM_IO){
            /* A short read may already have overwritten part of pixels.
             * Never revive the old PTS over that partially new payload. */
            s->slot=SLOT_EMPTY;
            atomic_store(&s->terminal,-1);
        }else if(s->slot==SLOT_WRITING){
            s->slot=had_ready?SLOT_READY:SLOT_EMPTY;
        }
        taskEXIT_CRITICAL(&s->lock);
        if(result==POCKET_VIDEO_STREAM_FUTURE){
            vTaskDelay(pdMS_TO_TICKS(WAIT_MS));continue;
        }
        if(result==POCKET_VIDEO_STREAM_SCANNING){
            vTaskDelay(1);continue;
        }
        if(result==POCKET_VIDEO_STREAM_END)
            atomic_store(&s->terminal,1);
        break;
    }
    taskENTER_CRITICAL(&s->lock);
    s->worker=NULL;
    taskEXIT_CRITICAL(&s->lock);
    ESP_LOGI("vidsd","worker terminal=%d stack_free=%u",
             atomic_load(&s->terminal),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    sd_media_read_lease_ack(&s->lease);
    vTaskDelete(NULL);
}
static void dispose(pocket_video_sd_stream *s){
    if(!sd_media_read_lease_close(&s->lease))return;
    free(s->pixels);free(s);
}
void pocket_video_sd_stream_reap(void){
    pocket_video_sd_stream **link=&quarantine;
    while(*link){
        pocket_video_sd_stream *s=*link;
        if(!sd_media_read_lease_acked(&s->lease)){link=&s->next;continue;}
        *link=s->next;dispose(s);
    }
}
pocket_video_sd_stream *pocket_video_sd_stream_start(const char *path,
    uint16_t width,uint16_t height,const char **code){
    pocket_video_sd_stream_reap();
    if(quarantine){
        if(code)*code="BUSY";
        return NULL;
    }
    if(!path||!width||!height||
       (uint32_t)width*height>POCKET_VIDEO_STREAM_MAX_PIXELS){
        if(code)*code="INVALID_ARGUMENT";
        return NULL;
    }
    pocket_video_sd_stream *s=calloc(1,sizeof *s);
    if(!s){
        if(code)*code="OUT_OF_MEMORY";
        return NULL;
    }
    s->bytes=(uint32_t)width*height*2;
    s->pixels=malloc(s->bytes);
    if(!s->pixels){
        free(s);
        if(code)*code="OUT_OF_MEMORY";
        return NULL;
    }
    s->lock=(portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    s->width=width;s->height=height;
    atomic_init(&s->cancel,false);atomic_init(&s->paused,false);
    atomic_init(&s->started,false);atomic_init(&s->terminal,0);
    uint32_t size=0;
    /* Persistent mode owns one FILE throughout. Together with the MP3
     * reader's transient REOPEN FILE this fits FS_MAX_HANDLES=2. */
    if(!pocket_fs_sd_read_lease_open(path,&s->lease,SD_MEDIA_LEASE_PERSISTENT,
                                     &size,code)){
        free(s->pixels);free(s);return NULL;
    }
    if(xTaskCreatePinnedToCore(worker,"vidsd",4096,s,4,&s->worker,0)!=pdPASS){
        sd_media_read_lease_ack(&s->lease);dispose(s);
        if(code)*code="OUT_OF_MEMORY";
        return NULL;
    }
    atomic_store(&s->started,true);
    return s;
}
void pocket_video_sd_stream_clock(pocket_video_sd_stream *s,uint64_t clock_us){
    if(!s)return;
    taskENTER_CRITICAL(&s->lock);s->clock_us=clock_us;
    taskEXIT_CRITICAL(&s->lock);
}
void pocket_video_sd_stream_pause(pocket_video_sd_stream *s,bool paused){
    if(s)atomic_store(&s->paused,paused);
}
bool pocket_video_sd_stream_borrow(pocket_video_sd_stream *s,
    const uint8_t **pixels,uint32_t *bytes,uint64_t *pts_us){
    if(!s||!pixels||!bytes||!pts_us)return false;
    bool ready=false;
    taskENTER_CRITICAL(&s->lock);
    if(s->slot==SLOT_READY&&atomic_load(&s->terminal)>=0){
        s->slot=SLOT_BORROWED;*pixels=s->pixels;*bytes=s->bytes;
        *pts_us=s->pts;ready=true;
    }
    taskEXIT_CRITICAL(&s->lock);
    return ready;
}
void pocket_video_sd_stream_release(pocket_video_sd_stream *s){
    if(!s)return;
    taskENTER_CRITICAL(&s->lock);
    if(s->slot==SLOT_BORROWED)s->slot=SLOT_EMPTY;
    taskEXIT_CRITICAL(&s->lock);
}
int pocket_video_sd_stream_state(const pocket_video_sd_stream *s){
    return s?atomic_load(&s->terminal):-1;
}
bool pocket_video_sd_stream_stop(pocket_video_sd_stream *s){
    if(!s)return true;
    int64_t started=esp_timer_get_time();
    atomic_store(&s->cancel,true);
    sd_media_read_lease_cancel(&s->lease);
    for(unsigned i=0;i<STOP_MS/WAIT_MS&&
         !sd_media_read_lease_acked(&s->lease);i++)
        vTaskDelay(pdMS_TO_TICKS(WAIT_MS));
    if(sd_media_read_lease_acked(&s->lease)){
        ESP_LOGI("vidsd","stop_ack_us=%u quarantined=0",
                 (unsigned)(esp_timer_get_time()-started));
        dispose(s);return true;
    }
    s->next=quarantine;quarantine=s;
    ESP_LOGW("vidsd","stop_ack_us=%u quarantined=1",
             (unsigned)(esp_timer_get_time()-started));
    return false;
}
