#ifndef POCKET_AV_PLAYBACK_SOURCE_H
#define POCKET_AV_PLAYBACK_SOURCE_H
#include "quickjs.h"

/* Session-scoped current-player source. Fields: state (U16), positionMs
 * (U32), durationMs (U32), underruns (U32), playing (BOOL).
 * All are invalid without an open
 * player. The service is created only when a caller requests its capability. */
JSValue pocket_av_playback_source(JSContext *,JSValueConst,int,JSValueConst *);
/* Owner-task service: publishes a complete snapshot when player identity,
 * state, elapsed second, duration, or underrun count changes. */
void pocket_av_playback_source_service(void);
/* Called after Kasane APP detach, when no source lease can remain pinned. */
void pocket_av_playback_source_reset(void);
#endif
