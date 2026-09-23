// Host test of the exact refill step used by the CPU0 SD worker.
#define _POSIX_C_SOURCE 200809L
#include "mp3_sd_reader.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    uint8_t data[7000];
    uint32_t size, calls, max_want;
    atomic_bool *cancel;
    bool delay, cancel_during, fail;
} fake_t;

static int32_t fake_read(void *ctx,uint32_t at,uint8_t *out,uint32_t want,int *err) {
    fake_t *f=ctx;
    f->calls++;
    if(want>f->max_want) f->max_want=want;
    if(f->delay) {
        struct timespec ts={.tv_sec=0,.tv_nsec=20000000};
        nanosleep(&ts,NULL);
    }
    if(f->cancel_during) atomic_store(f->cancel,true);
    if(f->fail) { *err=5; return -1; }
    if(at>=f->size) return 0;
    if(want>f->size-at) want=f->size-at;
    memcpy(out,f->data+at,want);
    return (int32_t)want;
}

int main(void) {
    uint8_t ring_bytes[SOUND_STREAM_SLOTS*SOUND_STREAM_SLOT_BYTES];
    sound_stream_t ring={.bytes=ring_bytes};
    atomic_bool cancel=false, revoked=false;
    fake_t fake={.size=5000,.cancel=&cancel};
    for(unsigned i=0;i<sizeof fake.data;i++) fake.data[i]=(uint8_t)(i*13u);
    mp3_sd_reader_t r={.packets=&ring,.at=0,.end=5000,
                       .cancel=&cancel,.revoked=&revoked,
                       .read_ctx=&fake,.read=fake_read};
    sound_stream_rewind(&ring);
    assert(mp3_sd_reader_step(&r)==MP3_SD_MORE);
    assert(mp3_sd_reader_step(&r)==MP3_SD_MORE);
    assert(mp3_sd_reader_step(&r)==MP3_SD_EOF);
    assert(r.at==5000&&fake.calls==3&&fake.max_want==2048);
    assert(atomic_load(&ring.eof));
    stream_read_t held={0};
    unsigned at=0;
    while(stream_take(&ring,&held,0)) {
        assert(memcmp(held.at,fake.data+at,held.bytes)==0);
        at+=held.bytes;
        stream_release(&ring,&held);
    }
    assert(at==5000);

    // Full ring parks without another read; draining one slot permits refill.
    sound_stream_rewind(&ring); r.at=0; r.end=7000; fake.size=7000;
    fake.calls=0;
    assert(mp3_sd_reader_step(&r)==MP3_SD_MORE);
    assert(mp3_sd_reader_step(&r)==MP3_SD_MORE);
    assert(mp3_sd_reader_step(&r)==MP3_SD_MORE);
    assert(mp3_sd_reader_step(&r)==MP3_SD_WAIT);
    assert(fake.calls==3);
    assert(stream_take(&ring,&held,0));
    stream_release(&ring,&held);
    assert(mp3_sd_reader_step(&r)==MP3_SD_EOF&&fake.calls==4);

    // A delayed in-flight read may finish after cancellation. Its bytes must
    // never be published, and a second step must not touch the source.
    sound_stream_rewind(&ring); r.at=0; r.end=5000;
    fake.calls=0; fake.delay=true; fake.cancel_during=true;
    assert(mp3_sd_reader_step(&r)==MP3_SD_CANCEL);
    assert(atomic_load(&ring.filled)==0&&r.at==0);
    assert(mp3_sd_reader_step(&r)==MP3_SD_CANCEL&&fake.calls==1);
    fake.delay=fake.cancel_during=false;
    atomic_store(&cancel,false);

    // Revocation before I/O is a distinct cancellation. A short physical
    // source and an I/O error are errors, never clean EOF.
    atomic_store(&revoked,true);
    assert(mp3_sd_reader_step(&r)==MP3_SD_CANCEL&&fake.calls==1);
    atomic_store(&revoked,false);
    fake.size=0;
    assert(mp3_sd_reader_step(&r)==MP3_SD_ERROR);
    assert(atomic_load(&ring.eof)&&atomic_load(&ring.filled)==0);
    sound_stream_rewind(&ring); fake.fail=true;
    assert(mp3_sd_reader_step(&r)==MP3_SD_ERROR);
    printf("mp3 SD reader: OK\n");
    return 0;
}
