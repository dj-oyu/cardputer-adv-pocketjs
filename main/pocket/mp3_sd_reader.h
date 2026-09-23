#pragma once
#include "sound_stream.h"

// Card-free, single-producer refill step shared by the device worker and host
// tests. A notification only asks the caller to retry; ring/cancel predicates
// here decide whether a read may happen.
typedef int32_t (*mp3_sd_read_fn)(void *,uint32_t,uint8_t *,uint32_t,int *);
typedef enum { MP3_SD_WAIT, MP3_SD_MORE, MP3_SD_EOF,
               MP3_SD_ERROR, MP3_SD_CANCEL } mp3_sd_step_t;
typedef struct {
    sound_stream_t *packets;
    uint32_t at, end;
    const atomic_bool *cancel, *revoked;
    void *read_ctx;
    mp3_sd_read_fn read;
} mp3_sd_reader_t;

static inline mp3_sd_step_t mp3_sd_reader_step(mp3_sd_reader_t *r) {
    if(atomic_load(r->cancel)||atomic_load(r->revoked)) return MP3_SD_CANCEL;
    if(r->at>=r->end) {
        atomic_store(&r->packets->eof,true);
        return MP3_SD_EOF;
    }
    uint8_t *slot=sound_stream_slot(r->packets);
    if(!slot) return MP3_SD_WAIT;
    uint32_t want=r->end-r->at;
    if(want>SOUND_STREAM_SLOT_BYTES) want=SOUND_STREAM_SLOT_BYTES;
    int err=0;
    int32_t got=r->read(r->read_ctx,r->at,slot,want,&err);
    // Cancellation during a blocking read discards its uncommitted slot.
    if(atomic_load(r->cancel)||atomic_load(r->revoked)) return MP3_SD_CANCEL;
    if(got<=0||err) {
        atomic_store(&r->packets->eof,true);
        return MP3_SD_ERROR;
    }
    r->at+=(uint32_t)got;
    sound_stream_publish(r->packets,(uint32_t)got,false);
    if(r->at>=r->end) {
        atomic_store(&r->packets->eof,true);
        return MP3_SD_EOF;
    }
    return MP3_SD_MORE;
}
