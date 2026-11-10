// The display task (docs/perf/two-core-pipeline.md, stage P1): present_frame()
// can run on a task of its own, pinned to CONFIG_POCKET_DISP_CORE, while the
// UI task waits for it. P1 joins at once, so nothing overlaps yet: this stage
// only proves the render path runs off the UI task with the same pixels and
// time. Compiled to nothing unless CONFIG_POCKET_DISP_TASK.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

typedef esp_err_t (*disp_job_fn)(void);

#ifdef CONFIG_POCKET_DISP_TASK
// Creates the task (static stack and TCB). Before the first disp_run().
esp_err_t disp_init(void);
// Runs `fn` on the display task and returns its result once it has finished.
// Runs it here instead when the task does not exist or `on_task` is false.
esp_err_t disp_run(disp_job_fn fn, bool on_task);
// The display task's free stack at its lowest so far, in bytes.
uint32_t disp_stack_free(void);
// The task's priority (FRAME_BD's P1 A/B, which also tries one above input).
void disp_set_priority(unsigned prio);
#else
static inline esp_err_t disp_init(void) { return ESP_OK; }
static inline esp_err_t disp_run(disp_job_fn fn, bool on_task) { (void)on_task; return fn(); }
static inline uint32_t disp_stack_free(void) { return 0; }
static inline void disp_set_priority(unsigned prio) { (void)prio; }
#endif
