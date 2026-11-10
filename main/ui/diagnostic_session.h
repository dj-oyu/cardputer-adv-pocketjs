#ifndef DIAGNOSTIC_SESSION_H
#define DIAGNOSTIC_SESSION_H
#include "diagnostic_command.h"
#include "diagnostic_lifecycle.h"

/* One serialized UI consumer, request/cancel producers only. The lifecycle
 * owner survives a failed callback. Never reconstruct a quarantined session.
 * A denied admission leaves the request pending; cancellation can suppress it.
 * Callbacks must attest completion, not just successful queue submission. */
typedef struct {
    diagnostic_command command;
    diagnostic_lifecycle lifecycle;
} diagnostic_session;
#define DIAGNOSTIC_SESSION_INIT { .command=DIAGNOSTIC_COMMAND_INIT }
typedef enum {
    DIAG_SESSION_WAITING, DIAG_SESSION_CANCELLED, DIAG_SESSION_COMPLETE,
    DIAG_SESSION_UNAVAILABLE, DIAG_SESSION_TERMINAL
} diagnostic_session_result;

static inline diagnostic_session_result diagnostic_session_pump(
    diagnostic_session *s,const diagnostic_lifecycle_ops *ops,bool admitted) {
    if(s->lifecycle.quarantined ||
       atomic_load(&s->command.state)==DIAG_CMD_TERMINAL)
        return DIAG_SESSION_TERMINAL;
    if(!admitted||!diagnostic_command_take(&s->command))return DIAG_SESSION_WAITING;
    diagnostic_begin_result begin=diagnostic_command_begin(&s->command);
    if(begin==DIAG_BEGIN_CANCELLED)return DIAG_SESSION_CANCELLED;
    if(begin!=DIAG_BEGIN_RUN)return DIAG_SESSION_WAITING;
    diagnostic_lifecycle_result life=diagnostic_lifecycle_run(&s->lifecycle,ops,true);
    bool safe=life==DIAG_LIFE_COMPLETE||life==DIAG_LIFE_UNAVAILABLE;
    diagnostic_finish_result finish=diagnostic_command_finish(&s->command,safe);
    if(!safe||finish==DIAG_FINISH_TERMINAL)return DIAG_SESSION_TERMINAL;
    if(life==DIAG_LIFE_UNAVAILABLE)return DIAG_SESSION_UNAVAILABLE;
    return finish==DIAG_FINISH_CANCELLED?DIAG_SESSION_CANCELLED:DIAG_SESSION_COMPLETE;
}
#endif
