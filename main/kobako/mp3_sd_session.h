#pragma once
#include "sd_media.h"
#include "sound_stream.h"

typedef struct mp3_sd_session mp3_sd_session_t;

// One SD playback owns both rings and the physical-path lease. A NULL result
// leaves no lease attached. The owner task alone calls these functions.
mp3_sd_session_t *mp3_sd_session_start(const char *path, uint32_t first,
                                        uint32_t end, const char **code);
sound_stream_t *mp3_sd_session_pcm(mp3_sd_session_t *s);
sound_stream_t *mp3_sd_session_packets(mp3_sd_session_t *s);
void mp3_sd_session_pause(mp3_sd_session_t *s, bool paused);
bool mp3_sd_session_fault(const mp3_sd_session_t *s);
uint32_t mp3_sd_session_position(const mp3_sd_session_t *s);
// Stop waits for ACK. If it times out or consumers still use either ring, the
// entire session is quarantined and a later owner-task service reaps it.
bool mp3_sd_session_stop(mp3_sd_session_t *s, bool audio_stopped,
                         bool decoder_stopped);
void mp3_sd_session_reap(void);
