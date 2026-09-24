#ifndef POCKET_AV_PLAYBACK_SOURCE_H
#define POCKET_AV_PLAYBACK_SOURCE_H
#include "quickjs.h"
#include "ui/kasane/ksn_source.h"

/* Session-scoped current-player source. Fields: state (U16), positionMs
 * (U32), durationMs (U32), underruns (U32), playing (BOOL), playerId (U32).
 * All are invalid without an open
 * player. The service is created only when a caller requests its capability. */
JSValue pocket_av_playback_source(JSContext *,JSValueConst,int,JSValueConst *);
/* Native consumer entry point. The returned registry stays live until APP
 * detach; callers must release all leases before the session reset. */
ksn_result pocket_av_playback_source_open(ksn_source_registry **registry,
                                           ksn_source_handle *handle);
/* Owner-task service: publishes a complete snapshot when player identity,
 * state, elapsed second, duration, or underrun count changes. */
void pocket_av_playback_source_service(void);
/* Called after Kasane APP detach, when no source lease can remain pinned. */
void pocket_av_playback_source_reset(void);
#endif
