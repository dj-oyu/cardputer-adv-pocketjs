#pragma once
#include <stdint.h>
#include <stddef.h>
/* Owner-task only. Never call from ISR or asynchronous LCD completion. */
enum dm_stage { DM_GUEST, DM_BRIDGE, DM_NATIVE, DM_PROC_BUILD, DM_VM, DM_POINTS, DM_RASTER,
                DM_TRANSFER, DM_OUTPUT, DM_OBSERVER, DM_STAGES };
typedef struct { uint64_t inclusive_us, self_us, calls, max_inclusive_us, max_self_us; } dm_counter;
typedef struct {
    dm_counter stage[DM_STAGES];
    uint64_t scalar_batches, pie_batches, sampled_heap_max, sampled_heap_cap;
    uint64_t contexts, continuations, completed_contexts, dispatch_failures;
    uint64_t transfer_bytes, transfer_success, transfer_failures, capture_samples;
    uint64_t clock_pair_min_us, clock_pair_max_us, scope_pairs, faults, output_refused;
} dm_snapshot;
#ifdef POCKET_DERBY_MEASURE
/* A scope automatically closes on every C return, including error returns. */
typedef struct { unsigned depth; int active; } dm_scope;
uint64_t dm_now_us(void);
dm_scope dm_enter(enum dm_stage stage);
void dm_leave(dm_scope *scope);
void dm_reset(void);
int dm_read(dm_snapshot *out); /* fails while scopes are active */
void dm_heap(size_t used,size_t cap);
void dm_batch(int pie);
void dm_calibrate(void); /* 32 empty timer pairs; not subtracted from results */
void dm_dispatch(int continuing,int failed);
void dm_present(int work_done,int capture);
void dm_transfer(size_t bytes,int failed,int capture);
void dm_output_reset(void);
void dm_refused(void);
size_t dm_format_record(char *line,size_t capacity,unsigned seq,uint64_t now,
                        const dm_snapshot *snapshot,int *overflow);
void dm_emit(size_t used,size_t cap); /* bounded 1 Hz / 300 attempts per reset */
#define DM_JOIN_(a,b) a##b
#define DM_JOIN(a,b) DM_JOIN_(a,b)
#define DM_SCOPE(stage) dm_scope DM_JOIN(dm_scope_,__LINE__) \
    __attribute__((cleanup(dm_leave)))=dm_enter(stage)
#define DM_BATCH(pie) dm_batch(pie)
#else
#define DM_SCOPE(stage) ((void)0)
#define DM_BATCH(pie) ((void)0)
#endif
