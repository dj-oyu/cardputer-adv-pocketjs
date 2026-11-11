// The display task (docs/perf/two-core-pipeline.md, stages P1/P2a):
// present_frame() can run on a task of its own, pinned to
// CONFIG_POCKET_DISP_CORE. disp_run() hands a job over and waits for it (P1).
// disp_start() hands it over and returns at once; disp_join() waits for it and
// then runs its finish step on the caller's task (P2a). Without
// CONFIG_POCKET_DISP_TASK, or with on_task false, the job runs in place inside
// disp_start() and the finish step still waits for disp_join(), so callers
// have one shape either way.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

typedef esp_err_t (*disp_job_fn)(void);
typedef void (*disp_finish_fn)(esp_err_t result);

// One job in flight at most; a start while one is in flight joins it first.
// UI task only, like disp_join().
void disp_start(disp_job_fn fn, disp_finish_fn finish, bool on_task);
// Waits for the job disp_start() handed over, then runs its finish step here.
// Returns at once when nothing is in flight, so it is safe on any path.
void disp_join(void);
// A job is in flight or its finish step has not run yet.
bool disp_busy(void);

#ifdef CONFIG_POCKET_DISP_TASK
// Creates the task (static stack and TCB). Before the first job.
esp_err_t disp_init(void);
// Runs `fn` on the display task and returns its result once it has finished.
// Runs it here instead when the task does not exist or `on_task` is false.
// Joins any job in flight first.
esp_err_t disp_run(disp_job_fn fn, bool on_task);
// The display task's free stack at its lowest so far, in bytes.
uint32_t disp_stack_free(void);
// The task's priority (FRAME_BD's P1 A/B, which also tries one above input).
void disp_set_priority(unsigned prio);
#else
static inline esp_err_t disp_init(void) { return ESP_OK; }
static inline esp_err_t disp_run(disp_job_fn fn, bool on_task) { (void)on_task; disp_join(); return fn(); }
static inline uint32_t disp_stack_free(void) { return 0; }
static inline void disp_set_priority(unsigned prio) { (void)prio; }
#endif
