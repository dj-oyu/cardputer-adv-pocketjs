#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// A permanent task whose stack lives in RTC fast memory (8 KB, APB clock).
//
// Why: the heap hands RTC fast to whoever asks once DIRAM is tight, and the
// Shiki JIT's code ran ~3x slower there (docs/kasane/shiki-jit.md sec.15). The
// system's own slow, mostly sleeping tasks (input polling, Wi-Fi, time sync)
// take it first, at boot, so nothing speed-sensitive can land in it.
//
// The stack comes from RTC fast, falling back to DIRAM if that is gone or
// absent. The TCB (~350 B, estimated) is always DIRAM: it is touched on every
// context switch and would take stack-sized room from a 8 KB pool. The task is
// pinned to core 0, the core that reaches RTC fast on every chip, and never
// deleted: nothing is freed, so a stack is never reused behind a live task.
// Returns NULL (and frees what it took) when memory is short.
TaskHandle_t rtc_task_create(TaskFunction_t fn, const char *name,
                             uint32_t stack_bytes, UBaseType_t prio);
