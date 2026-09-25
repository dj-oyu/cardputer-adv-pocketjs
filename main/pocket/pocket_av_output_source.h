#ifndef POCKET_AV_OUTPUT_SOURCE_H
#define POCKET_AV_OUTPUT_SOURCE_H
#include "quickjs.h"
#include "ui/kasane/ksn_source.h"
#include <stdbool.h>
#include <stdint.h>

/* Session-scoped capability. Fields: 0 = HH:MM:SS text, 1 = logical
 * output frame position (U32), 2 = cumulative starved blocks (U32),
 * 3 = stream id (U32). All fields are invalid outside an active stream,
 * restoring their consumers' base values.
 * The source owns its pool and registry; Kasane only sees a typed provider.
 * outputSource({sampleMs: 32..1000}) configures the audio-task observation
 * period before the source is first created; omitted means 1000 ms. A later
 * call may retrieve the same capability but cannot change its period. */
JSValue pocket_av_output_source(JSContext *,JSValueConst,int,JSValueConst *);
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
/* Diagnostic native consumer: return the existing producer without creating
 * or changing its sample period. Valid until APP detach. */
ksn_result pocket_av_output_source_existing(ksn_source_registry **registry,
                                             ksn_source_handle *handle);
#endif
/* Called on the owner task every service turn; retries a skipped final
 * invalidation once the audio task has handed back the stream. */
void pocket_av_output_source_service(int32_t current_stream_id);
/* Disable new audio-task callbacks before stopping playback. */
void pocket_av_output_source_suspend(void);
/* After Kasane APP detach: unregister and free only if the audio task stopped.
 * False means storage was deliberately retained because a producer or reader
 * can still hold it. New outputSource calls return BUSY for the rest of this
 * boot; without a quiescence proof, reuse would overwrite the live callback. */
bool pocket_av_output_source_reset(bool audio_stopped);
#endif
