#ifndef SYS_WALL_H
#define SYS_WALL_H
#include "sys_state.h"
#include "sys_notify.h"
#define SYS_WALL_RULES 5
/* Native owner bindings only. Pointers outlive the service; never bind guest
 * memory. The persistence adapter owns watermarks and writes them to storage.
 * due!=NULL selects a one-shot epoch; otherwise minute/offset select daily.
 * Configuration mutation must call invalidate, after any persistence rollback. */
typedef struct {
    const uint32_t *due;
    uint32_t *last;
    const int32_t *minute,*offset;
    const char *label;
} sys_wall_rule;
typedef struct {
    sys_wall_rule rules[SYS_WALL_RULES];
    uint64_t next;
    uint32_t owner,clock_revision,notice_revision;
    bool invalid,blocked,changed;
} sys_wall;
void sys_wall_invalidate(sys_wall *);
uint64_t sys_wall_deadline(const sys_wall *,const sys_state *,const sys_notify *);
void sys_wall_step(sys_wall *,const sys_state *,sys_notify *,uint64_t now_us);
bool sys_wall_take_changed(sys_wall *);
/* Compatibility evaluator shared by adapters and tests. Returns next UTC
 * second, UINT64_MAX when dormant; successful post updates *last. */
uint64_t sys_wall_evaluate(const sys_wall_rule *,sys_notify *,uint32_t owner,
                           uint32_t utc,bool *blocked);
#endif
