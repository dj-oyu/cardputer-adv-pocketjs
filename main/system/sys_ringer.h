#ifndef SYS_RINGER_H
#define SYS_RINGER_H
#include "sys_notify.h"
/* Owner-only, no audio calls. A poll grants at most one tone; late callers
 * never replay missed tones. IDs are process-unique notification identities. */
typedef struct { uint64_t until,next,order; uint32_t id; } sys_ringer;
uint64_t sys_ringer_deadline(const sys_ringer *,const sys_notify *);
bool sys_ringer_poll(sys_ringer *,const sys_notify *,uint64_t now_us);
#endif
