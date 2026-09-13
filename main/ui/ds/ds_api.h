#ifndef DS_API_H
#define DS_API_H
#include "ds_types.h"
/* One shared vtable, not one per command. Guest adapters expose APP only.
 * All calls run on the owner task, without re-entering JS. */
typedef struct {
    ds_result (*begin)(void *,ds_update_mode,ds_tx *);
    ds_result (*background)(void *,ds_tx,ds_rgba);
    ds_result (*add)(void *,ds_tx,const ds_draw *,ds_ref *);
    ds_result (*change)(void *,ds_tx,ds_ref,const ds_change *);
    ds_result (*animate)(void *,ds_tx,const ds_motion *,ds_animation *);
    ds_result (*stop)(void *,ds_tx,ds_animation);
    ds_result (*end)(void *,ds_tx);
    void (*abort)(void *,ds_tx);
    ds_limits (*limits)(void *);
    ds_stats (*stats)(void *);
} ds_api;
typedef struct { const ds_api *ops; void *ctx; } ds_client;
/* A client is bound to APP or SYSTEM when the host creates it; begin cannot
 * select another layer. One builder/submission is shared across both layers.
 * REPLACE clears its bound layer only.
 * Tokens validate session, layer and generation. Errors poison until abort.
 * abort is idempotent. end seals, but does NOT acknowledge LCD presentation.
 * References are usable inside their transaction; publish app handles only
 * after successful end. BUSY must preserve the caller's pending domain state.
 * A submitted state must be presented/discarded before the next begin. */
typedef struct { uint64_t next_us; bool ready; } ds_schedule;
typedef struct {
    ds_schedule (*schedule)(void *,uint64_t now_us);
    ds_result (*present)(void *,uint64_t now_us);
    void (*set_hidden)(void *,bool,uint64_t now_us);
    void (*set_reduce_motion)(void *,bool);
    void (*reset)(void *);
} ds_host_api;
/* UINT64_MAX means no deadline. present consumes the strip synchronously.
 * IO failure keeps the old baseline and schedules full retry. reset cancels
 * all transactions/tracks/resources without invoking JS. */
#endif
