#ifndef SYS_TIMER_H
#define SYS_TIMER_H
#include "sys_notify.h"
#define SYS_TIMER_SLOTS 4
#define SYS_TIMER_KEY 16
typedef struct {
    uint64_t due_us;
    uint32_t owner;
    char key[SYS_TIMER_KEY+1],label[SYS_NOTICE_LABEL+1];
    bool blocked;
} sys_timer_record;
typedef struct {
    sys_timer_record records[SYS_TIMER_SLOTS];
    uint64_t deadline;
    bool changed;
} sys_timer;
/* Owner-only, zero-init. due=0 cancels idempotently. A full notification store
 * retains due timers; retry requires a notification change, not periodic wake. */
sys_notice_result sys_timer_set(sys_timer *,uint32_t owner,const char *key,const char *label,uint64_t due_us);
bool sys_timer_read(const sys_timer *,uint32_t owner,const char *key,sys_timer_record *out);
void sys_timer_step(sys_timer *,sys_notify *,uint64_t now_us,bool notification_changed);
void sys_timer_release_owner(sys_timer *,uint32_t owner);
uint64_t sys_timer_deadline(const sys_timer *);
bool sys_timer_take_changed(sys_timer *);
#endif
