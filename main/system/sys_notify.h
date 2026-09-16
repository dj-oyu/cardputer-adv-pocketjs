#ifndef SYS_NOTIFY_H
#define SYS_NOTIFY_H
#include <stdbool.h>
#include <stdint.h>
#define SYS_NOTICE_SLOTS 9
#define SYS_NOTICE_WAITING 8
#define SYS_NOTICE_LABEL 24
typedef enum { NOTICE_FREE, NOTICE_QUEUED, NOTICE_ACTIVE, NOTICE_SNOOZED } sys_notice_phase;
typedef enum { NOTICE_OK, NOTICE_INVALID, NOTICE_FULL, NOTICE_GONE } sys_notice_result;
typedef struct {
    uint64_t wake_us,expires_us,order;
    uint32_t id,owner,key;
    char label[SYS_NOTICE_LABEL+1];
    uint8_t phase;
} sys_notice;
typedef struct {
    sys_notice records[SYS_NOTICE_SLOTS];
    uint64_t next_order,deadline;
    uint32_t revision;
    bool changed;
    uint8_t active_slot;
} sys_notify;
/* Zero-init, one owner, no heap/callbacks. IDs are process-unique; exhausted
 * IDs fail closed. Snapshots are copied, never borrowed across mutations.
 * key=0 disables deduplication. A duplicate returns the existing ID unchanged.
 * expires_us=0 disables TTL; otherwise it is an absolute monotonic deadline. */
sys_notice_result sys_notify_post(sys_notify *,uint32_t owner,uint32_t key,
    const char *label,uint64_t expires_us,uint32_t *id);
bool sys_notify_active(const sys_notify *,sys_notice *out);
sys_notice_result sys_notify_ack(sys_notify *,uint32_t owner,uint32_t id);
sys_notice_result sys_notify_cancel(sys_notify *,uint32_t owner,uint32_t id);
sys_notice_result sys_notify_snooze(sys_notify *,uint32_t owner,uint32_t id,uint64_t wake_us);
void sys_notify_release_owner(sys_notify *,uint32_t owner);
void sys_notify_step(sys_notify *,uint64_t now_us);
uint64_t sys_notify_deadline(const sys_notify *);
bool sys_notify_take_changed(sys_notify *);
#endif
