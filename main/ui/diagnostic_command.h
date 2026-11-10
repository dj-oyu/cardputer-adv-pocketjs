#ifndef DIAGNOSTIC_COMMAND_H
#define DIAGNOSTIC_COMMAND_H
#include <stdbool.h>
#include <stdatomic.h>

/* Internal opt-in diagnostic coordination, not an application-control API.
 * Producers request/cancel; exactly one consumer takes, begins and finishes.
 * Construct before sharing, keep alive through all calls, never reset a live
 * instance (especially a terminal one). No payload publication is implied.
 * Cancellation after begin is deferred: the consumer must finish its normal
 * completion/repair boundary before finish. This helper cannot abort I/O.
 * Pointers are trusted/non-NULL; callers must not write state themselves. */
typedef enum {
    DIAG_CMD_IDLE, DIAG_CMD_PENDING, DIAG_CMD_CLAIMED, DIAG_CMD_RUNNING,
    DIAG_CMD_CANCEL_CLAIMED, DIAG_CMD_CANCEL_RUNNING, DIAG_CMD_TERMINAL
} diagnostic_command_state;
typedef struct { atomic_uint state; } diagnostic_command;
#define DIAGNOSTIC_COMMAND_INIT { ATOMIC_VAR_INIT(DIAG_CMD_IDLE) }
typedef enum { DIAG_REQUEST_QUEUED, DIAG_REQUEST_COALESCED,
    DIAG_REQUEST_BUSY, DIAG_REQUEST_TERMINAL } diagnostic_request_result;
typedef enum { DIAG_CANCEL_IDLE, DIAG_CANCEL_SUPPRESSED,
    DIAG_CANCEL_DEFERRED, DIAG_CANCEL_TERMINAL } diagnostic_cancel_result;
typedef enum { DIAG_BEGIN_RUN, DIAG_BEGIN_CANCELLED,
    DIAG_BEGIN_NOT_CLAIMED } diagnostic_begin_result;
typedef enum { DIAG_FINISH_COMPLETE, DIAG_FINISH_CANCELLED,
    DIAG_FINISH_TERMINAL, DIAG_FINISH_NOT_RUNNING } diagnostic_finish_result;

static inline diagnostic_request_result diagnostic_command_request(diagnostic_command *c) {
    unsigned s=DIAG_CMD_IDLE;
    if(atomic_compare_exchange_strong(&c->state,&s,DIAG_CMD_PENDING))return DIAG_REQUEST_QUEUED;
    return s==DIAG_CMD_PENDING?DIAG_REQUEST_COALESCED:
        s==DIAG_CMD_TERMINAL?DIAG_REQUEST_TERMINAL:DIAG_REQUEST_BUSY;
}
static inline diagnostic_cancel_result diagnostic_command_cancel(diagnostic_command *c) {
    unsigned s=atomic_load(&c->state);
    for(;;) {
        unsigned next;
        if(s==DIAG_CMD_PENDING)next=DIAG_CMD_IDLE;
        else if(s==DIAG_CMD_CLAIMED)next=DIAG_CMD_CANCEL_CLAIMED;
        else if(s==DIAG_CMD_RUNNING)next=DIAG_CMD_CANCEL_RUNNING;
        else return s==DIAG_CMD_TERMINAL?DIAG_CANCEL_TERMINAL:
            s==DIAG_CMD_IDLE?DIAG_CANCEL_IDLE:DIAG_CANCEL_DEFERRED;
        if(atomic_compare_exchange_weak(&c->state,&s,next))
            return next==DIAG_CMD_IDLE?DIAG_CANCEL_SUPPRESSED:DIAG_CANCEL_DEFERRED;
    }
}
static inline bool diagnostic_command_take(diagnostic_command *c) {
    unsigned s=DIAG_CMD_PENDING;
    return atomic_compare_exchange_strong(&c->state,&s,DIAG_CMD_CLAIMED);
}
static inline diagnostic_begin_result diagnostic_command_begin(diagnostic_command *c) {
    unsigned s=DIAG_CMD_CLAIMED;
    if(atomic_compare_exchange_strong(&c->state,&s,DIAG_CMD_RUNNING))return DIAG_BEGIN_RUN;
    if(s==DIAG_CMD_CANCEL_CLAIMED&&
       atomic_compare_exchange_strong(&c->state,&s,DIAG_CMD_IDLE))return DIAG_BEGIN_CANCELLED;
    return DIAG_BEGIN_NOT_CLAIMED;
}
/* success means work AND any required restoration have completed. Failure is
 * absorbing, including on a repeated finish; no helper provides recovery. */
static inline diagnostic_finish_result diagnostic_command_finish(diagnostic_command *c,bool success) {
    unsigned s=atomic_load(&c->state);
    for(;;) {
        if(s==DIAG_CMD_TERMINAL)return DIAG_FINISH_TERMINAL;
        if(s!=DIAG_CMD_RUNNING&&s!=DIAG_CMD_CANCEL_RUNNING)return DIAG_FINISH_NOT_RUNNING;
        if(atomic_compare_exchange_weak(&c->state,&s,success?DIAG_CMD_IDLE:DIAG_CMD_TERMINAL))
            return !success?DIAG_FINISH_TERMINAL:
                s==DIAG_CMD_CANCEL_RUNNING?DIAG_FINISH_CANCELLED:DIAG_FINISH_COMPLETE;
    }
}
#endif
