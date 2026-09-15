#ifndef SYS_STATE_H
#define SYS_STATE_H
#include <stdbool.h>
#include <stdint.h>
#define SYS_SUBSCRIPTIONS 8
#define SYS_POWER UINT32_C(1)
#define SYS_CLOCK_CONFIG UINT32_C(2)
#define SYS_TOPICS (SYS_POWER|SYS_CLOCK_CONFIG)
#define SYS_NEVER UINT64_MAX
#define SYS_POWER_PERIOD_US UINT64_C(1000000)
typedef struct { uint32_t value; } sys_sub;
typedef enum { SYS_OK, SYS_INVALID, SYS_FULL, SYS_STALE } sys_result;
typedef struct { uint32_t id,interest,pending; } sys_subscription;
typedef struct {
    uint64_t sampled_at;
    int32_t millivolts,error;
    bool sampled,valid;
} sys_power_state;

typedef enum { SYS_CLOCK_NONE, SYS_CLOCK_RTC, SYS_CLOCK_SNTP, SYS_CLOCK_PC } sys_clock_source;
typedef struct {
    int64_t seconds;
    uint64_t mono_us;
    int32_t microseconds;
    sys_clock_source source;
} sys_clock_anchor;
typedef struct {
    int64_t seconds;
    int32_t microseconds, utc_offset;
    uint32_t revision;
    sys_clock_source source;
    bool valid, trusted;
} sys_clock_state;
typedef struct {
    sys_subscription subscriptions[SYS_SUBSCRIPTIONS];
    sys_power_state power;
    sys_clock_anchor clock, pc_clock;
    int32_t utc_offset;
    uint32_t clock_revision;
    uint64_t power_next;
    int32_t announced_mv,announced_error;
    uint8_t power_users;
    bool announced,announced_valid,refresh;
} sys_state;
/* Zero-initialize once. Operations run on one owner, without allocations or
 * consumer callbacks. Read only copies the cache; it never accesses ADC. */
sys_result sys_subscribe(sys_state *,uint32_t interest,sys_sub *out);
sys_result sys_set_interest(sys_state *,sys_sub,uint32_t interest);
sys_result sys_unsubscribe(sys_state *,sys_sub);
sys_result sys_poll(sys_state *,sys_sub,uint32_t *changed);
/* Owner-only. Snapshot advances an anchor without mutation or wall-clock IO.
 * PC is format-validated input, not authenticated time; RTC/SNTP wins. */
bool sys_clock_snapshot(const sys_state *,uint64_t now_us,sys_clock_state *out);
sys_result sys_clock_update(sys_state *,sys_clock_anchor);
sys_result sys_clock_offer_pc(sys_state *,uint32_t seconds,uint64_t now_us);
sys_result sys_clock_timezone(sys_state *,int32_t utc_offset);
bool sys_power_read(const sys_state *,sys_power_state *out);
void sys_power_refresh(sys_state *);
uint64_t sys_power_deadline(const sys_state *);
/* Owner port must not reenter the service or invoke application code. */
typedef sys_power_state (*sys_power_port)(void *ctx,uint64_t now_us);
void sys_power_step(sys_state *,uint64_t now_us,sys_power_port,void *ctx);
#endif
