#ifndef KSN_API_H
#define KSN_API_H
#include "ksn_types.h"
/* One shared vtable, not one per command. Guest adapters expose APP only.
 * All calls run on the owner task, without re-entering JS. */
typedef struct {
    ksn_result (*begin)(void *,ksn_update_mode,ksn_tx *);
    ksn_result (*background)(void *,ksn_tx,ksn_rgba);
    ksn_result (*add)(void *,ksn_tx,const ksn_draw *,ksn_ref *);
    ksn_result (*change)(void *,ksn_tx,ksn_ref,const ksn_change *);
    ksn_result (*animate)(void *,ksn_tx,const ksn_motion *,ksn_animation *);
    ksn_result (*stop)(void *,ksn_tx,ksn_animation);
    ksn_result (*end)(void *,ksn_tx);
    void (*abort)(void *,ksn_tx);
    ksn_limits (*limits)(void *);
    ksn_stats (*stats)(void *);
} ksn_api;
typedef struct { const ksn_api *ops; void *ctx; } ksn_client;
/* A client is bound to APP or SYSTEM when the host creates it; begin cannot
 * select another layer. One builder/submission is shared across both layers.
 * REPLACE clears its bound layer only. add is valid only in REPLACE;
 * PATCH changes values and preserves command topology and reference identity.
 * Tokens validate session, layer and generation. Errors in the owning
 * transaction poison until abort; foreign/stale tokens leave it untouched.
 * abort is idempotent. end seals, but does NOT acknowledge LCD presentation.
 * References are usable inside their transaction. Successful end retains
 * candidate handles; promote them only after PRESENTED, invalidate them on
 * discard. New adapters use ksn_view.h for coordinated result/abort handling.
 * BUSY must preserve the caller's pending domain state.
 * A submitted state must be presented/discarded before the next begin. */
typedef struct { uint64_t next_us; bool ready; } ksn_schedule;
typedef struct {
    ksn_schedule (*schedule)(void *,uint64_t now_us);
    ksn_result (*present)(void *,uint64_t now_us);
    void (*set_hidden)(void *,bool,uint64_t now_us);
    void (*set_reduce_motion)(void *,bool);
    void (*reset)(void *);
} ksn_host_api;
/* UINT64_MAX means no deadline. present consumes the strip synchronously.
 * IO failure keeps the old baseline and schedules full retry. reset cancels
 * all transactions/tracks/resources without invoking JS. */
#endif
