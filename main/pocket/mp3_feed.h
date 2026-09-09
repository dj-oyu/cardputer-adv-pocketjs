#pragma once
#include "sound_stream.h"
typedef enum { MP3_FEED_OK=0, MP3_FEED_NOMEM, MP3_FEED_BUSY } mp3_feed_start_t;
mp3_feed_start_t mp3_feed_start(sound_stream_t *pcm, sound_stream_t *input,
                                uint32_t skip_frames);
bool mp3_feed_stop(void);
uint32_t mp3_feed_faults(void);
uint32_t mp3_feed_frames(void);
uint32_t mp3_feed_progress(void);
