#ifndef DIAGNOSTIC_LIFECYCLE_H
#define DIAGNOSTIC_LIFECYCLE_H
#include <stdbool.h>
#include <stddef.h>

/* Internal diagnostic owner policy, independent of command transport.
 * Zero-initialize once. All calls and callbacks are serialized, non-reentrant
 * on one owner thread. Callbacks/context/state are trusted and nonaliasing;
 * callbacks must not mutate this state or release the acquired object.
 * A failure can leave physical output and borrows uncertain. Retain ownership
 * and stop further presentation; this helper provides no recovery/reset.
 * It does not make display writes atomic or guarantee restoration succeeds. */
typedef struct { void *owner; bool quarantined; } diagnostic_lifecycle;
typedef struct {
    void *context;
    void *(*acquire)(void *);
    /* True attests successful final completion and release of prior borrows,
     * not merely descriptor availability. */
    bool (*drain)(void *);
    /* True attests successful work completion and release of its borrows.
     * The prior drain does not drain work: restoration starts immediately. */
    bool (*work)(void *,void *);
    /* True attests complete restoration, final output completion, and release
     * of ALL work/restoration borrows before the acquired object is freed. */
    bool (*restore)(void *);
    void (*release)(void *,void *);
} diagnostic_lifecycle_ops;
typedef enum { DIAG_LIFE_COMPLETE, DIAG_LIFE_REFUSED, DIAG_LIFE_UNAVAILABLE,
    DIAG_LIFE_QUARANTINED, DIAG_LIFE_INVALID } diagnostic_lifecycle_result;

static inline diagnostic_lifecycle_result diagnostic_lifecycle_run(
    diagnostic_lifecycle *s,const diagnostic_lifecycle_ops *ops,bool admitted) {
    if(!s)return DIAG_LIFE_INVALID;
    if(s->quarantined)return DIAG_LIFE_QUARANTINED;
    if(!ops||!ops->acquire||!ops->drain||!ops->work||!ops->restore||!ops->release)
        return DIAG_LIFE_INVALID;
    if(!admitted||s->owner)return DIAG_LIFE_REFUSED;
    s->owner=ops->acquire(ops->context);
    if(!s->owner)return DIAG_LIFE_UNAVAILABLE;
    if(!ops->drain(ops->context)||!ops->work(ops->context,s->owner)||!ops->restore(ops->context)) {
        s->quarantined=true;
        return DIAG_LIFE_QUARANTINED;
    }
    ops->release(ops->context,s->owner);
    s->owner=NULL;
    return DIAG_LIFE_COMPLETE;
}
#endif
