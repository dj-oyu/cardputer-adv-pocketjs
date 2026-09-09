#ifndef FP_TICKET_H
#define FP_TICKET_H
#include <stdint.h>

#define FP_TICKET_CORES 2u
#define FP_TICKET_SLOTS 16u
#define FP_TICKET_FULL_LOAD 1000000u
#define FP_TICKET_CORE_0 1u
#define FP_TICKET_CORE_1 2u
#define FP_TICKET_ANY_CORE 3u
#define FP_TICKET_USES_FP 1u

typedef enum {
    FP_TICKET_OK=0, FP_TICKET_INVALID, FP_TICKET_NO_CAPACITY,
    FP_TICKET_NO_SLOT, FP_TICKET_STALE, FP_TICKET_ID_EXHAUSTED,
    FP_TICKET_FP_BUSY
} fp_ticket_result_t;

typedef struct {
    // CPU time per activation, including conversion/glue, excluding I/O waits.
    // Admission rounds runtime/period upward. It is not a deadline guarantee.
    uint32_t runtime_us, period_us;
    uint8_t allowed_cores;
    uint8_t flags; // Zero for non-FP tasks; FP_TICKET_USES_FP otherwise.
    // Metadata for a future task creator, not a right to evict reservations.
    uint32_t priority;
} fp_ticket_request_t;

typedef struct {
    uint8_t core;
    uint32_t load_ppm;
    fp_ticket_request_t request;
} fp_ticket_info_t;

typedef struct fp_ticket_manager fp_ticket_manager_t;
typedef struct {
    const fp_ticket_manager_t *owner;
    uint32_t id;
} fp_ticket_t;

typedef struct {
    const fp_ticket_request_t *request;
    uint32_t id; // Identifiers are never recycled.
} fp_ticket_slot_t;

// Caller-owned fixed storage; fields are private to fp_ticket.c.
// No heap, global state, FreeRTOS calls, task creation or affinity changes.
// ALL calls on one manager, including queries, must be serialized by the
// owner task or an external mutex. Do not use a preemptible busy-wait lock.
struct fp_ticket_manager {
    uint32_t capacity_ppm[FP_TICKET_CORES];
    uint32_t next_id;
    uint16_t live_mask, core1_mask, fp_mask;
    fp_ticket_slot_t slots[FP_TICKET_SLOTS];
};

typedef struct {
    uint32_t capacity_ppm[FP_TICKET_CORES], used_ppm[FP_TICKET_CORES];
    uint32_t reservations[FP_TICKET_CORES];
    uint32_t non_fp_reservations[FP_TICKET_CORES];
    uint8_t fp_cores;
} fp_ticket_snapshot_t;

// Initialize ONCE. Keep the manager at the same address, alive longer than all
// tickets. Do not copy/reinitialize it while old tickets can be used.
// A zero capacity disables a core; disabling both is invalid.
fp_ticket_result_t fp_ticket_init(fp_ticket_manager_t *manager,
                                 uint32_t core0_ppm, uint32_t core1_ppm);
// Reserve before task creation or first FP use. Lowest projected load/capacity
// wins; non-FP ties prefer fewer non-FP tasks, then Core 0.
// At most one live FP reservation per core, including sleeping tasks.
// Request storage must remain alive and immutable until release (static const
// permits flash storage). Failure leaves manager and output unchanged.
fp_ticket_result_t fp_ticket_acquire(fp_ticket_manager_t *manager,
                                    const fp_ticket_request_t *request,
                                    fp_ticket_t *ticket);
fp_ticket_result_t fp_ticket_get(const fp_ticket_manager_t *manager,
                                fp_ticket_t ticket, fp_ticket_info_t *info);
// Release after task termination, or roll back failed task creation. The
// reservation lasts for the task lifetime, not for a single FP operation.
fp_ticket_result_t fp_ticket_release(fp_ticket_manager_t *manager,
                                    fp_ticket_t ticket);
fp_ticket_result_t fp_ticket_snapshot(const fp_ticket_manager_t *manager,
                                     fp_ticket_snapshot_t *snapshot);
#endif
