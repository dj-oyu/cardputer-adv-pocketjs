#ifndef POCKET_AV_OUTPUT_SOURCE_H
#define POCKET_AV_OUTPUT_SOURCE_H
#include "quickjs.h"
#include <stdbool.h>
#include <stdint.h>

/* Session-scoped capability. Field 0 is an eight-byte HH:MM:SS text value
 * while an output stream is active; no stream restores the consumer's base.
 * The source owns its pool and registry; Kasane only sees a typed provider. */
JSValue pocket_av_output_source(JSContext *,JSValueConst,int,JSValueConst *);
/* Called on the owner task every service turn; retries a skipped final
 * invalidation once the audio task has handed back the stream. */
void pocket_av_output_source_service(int32_t current_stream_id);
/* Disable new audio-task callbacks before stopping playback. */
void pocket_av_output_source_suspend(void);
/* After Kasane APP detach: unregister and free only if the audio task stopped. */
void pocket_av_output_source_reset(bool audio_stopped);
#endif
