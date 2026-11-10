#ifndef DIAGNOSTIC_CONTROL_H
#define DIAGNOSTIC_CONTROL_H
#include "diagnostic_session.h"
#include <stdint.h>

/* Domain controller, not a wire codec or authentication boundary. Construct
 * once before tasks; boot/enabled/session are immutable. REQUEST and CANCEL
 * have one producer (input task); UI alone pumps callbacks. Snapshot readers
 * may run concurrently. IDs are immutable after release-published used count;
 * phase/flags/cancel/start facts share one lock-free atomic word per receipt.
 * Manual request producers honor busy(); physical stop may mark cancel.
 * No eviction/reset/recovery: after eight IDs, new work is refused this boot. */
enum { DC_SLOTS=8, DC_ID_BYTES=16 };
/* WORK_DONE means the callback returned, including failure; only the terminal
 * state plus all required flags proves safe completion. */
enum { DC_WORK_DONE=1, DC_DRAINED=2, DC_HOME=4, DC_BORROWS_RELEASED=8,
       DC_OWNER_RELEASED=16, DC_NOT_STARTED=32 };
typedef enum { DC_PENDING, DC_RUNNING, DC_CANCEL_REQUESTED, DC_COMPLETED,
               DC_CANCELLED, DC_REFUSED, DC_QUARANTINED } diagnostic_control_state;
typedef struct {
    diagnostic_control_state state;
    uint8_t flags;
    bool known;
} diagnostic_control_snapshot;
typedef struct { uint8_t id[DC_ID_BYTES]; atomic_uint word; } diagnostic_control_receipt;
typedef struct {
    uint64_t boot;
    diagnostic_session *session;
    atomic_uint used,active; /* active is slot+1, zero means no reserved work */
    bool enabled;
    diagnostic_control_receipt receipt[DC_SLOTS];
} diagnostic_control;
typedef enum { DC_OWNER_NONE, DC_OWNER_WAITING, DC_OWNER_SAFE_END,
               DC_OWNER_REFUSED, DC_OWNER_TERMINAL } diagnostic_control_owner_result;

bool diagnostic_control_init(diagnostic_control *,uint64_t boot,bool enabled,diagnostic_session *);
diagnostic_control_snapshot diagnostic_control_request(diagnostic_control *,uint64_t expected_boot,const uint8_t id[DC_ID_BYTES]);
diagnostic_control_snapshot diagnostic_control_status(const diagnostic_control *,uint64_t expected_boot,const uint8_t id[DC_ID_BYTES]);
diagnostic_control_snapshot diagnostic_control_cancel(diagnostic_control *,uint64_t expected_boot,const uint8_t id[DC_ID_BYTES]);
/* Physical force-stop also marks the active receipt; it does not stand in for
 * a typed request, alter any other ID, or release a diagnostic reservation. */
void diagnostic_control_local_cancel(diagnostic_control *);
bool diagnostic_control_busy(const diagnostic_control *);
/* UI only. admission is freshly evaluated by the actual UI owner. A cancelled
 * pre-work run still acquires/drains/restores/releases before 0x3e is safe.
 * With a failed admission it is REFUSED 0x20, never a false HOME receipt. */
diagnostic_control_owner_result diagnostic_control_pump(diagnostic_control *,const diagnostic_lifecycle_ops *,bool admitted);
#endif
