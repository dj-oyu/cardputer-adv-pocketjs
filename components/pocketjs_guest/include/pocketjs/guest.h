#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "pocketjs/vm_sched.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POCKETJS_GUEST_ABI_VERSION 1U
#define POCKETJS_GUEST_MAX_TOUCHES 8U

typedef struct pocketjs_guest pocketjs_guest_t;

typedef struct {
  size_t struct_size;
  size_t heap_limit;
  size_t stack_limit;
  bool prefer_psram;
} pocketjs_guest_config_t;

typedef struct {
  size_t struct_size;
  uint32_t buttons;
  /** (x << 8) | y, with both normalized axes in 0..255 and 128 centered. */
  uint32_t analog;
  const uint32_t *touches;
  const int32_t *touch_hits;
  size_t touch_count;
} pocketjs_guest_frame_t;

typedef struct {
  size_t struct_size;
  uint32_t frames;
  uint32_t frame_errors;
  uint32_t jobs;
  size_t heap_used;
  size_t heap_limit;
  /* L1 (docs/vm-L1-design.md). struct_size guards the ABI: a caller built
   * against the shorter struct asks for the shorter struct and gets it. */
  uint32_t yields;         /* drains cut by the budget */
  uint32_t continuations;  /* turns that began by finishing a previous drain */
  bool jobs_pending;       /* the last drain ended with the queue non-empty */
  bool jobs_dropped;       /* jobs were queued when the session ended */
} pocketjs_guest_stats_t;

void pocketjs_guest_config_defaults(pocketjs_guest_config_t *config);

esp_err_t pocketjs_guest_create(const pocketjs_guest_config_t *config,
                                pocketjs_guest_t **out_guest);

/** Evaluate one global IIFE. Surfaces must be installed before this call. */
esp_err_t pocketjs_guest_eval(pocketjs_guest_t *guest, const char *source,
                              size_t source_size, const char *label);

/** Call globalThis.frame(...) once and drain every pending Promise job. */
esp_err_t pocketjs_guest_frame(pocketjs_guest_t *guest,
                               const pocketjs_guest_frame_t *frame);

/** Arm this turn's job budget (docs/vm-L1-design.md sec.1.3). NULL, or a
 * budget with limit_us <= 0, restores the pre-L1 "drain until empty"
 * behaviour exactly. Call once per turn, before any call into the guest. */
void pocketjs_guest_budget(pocketjs_guest_t *guest, const vm_budget_t *budget);

/** True when the last drain stopped on the budget with jobs still queued.
 * The host must finish them with pocketjs_guest_continue() at the top of the
 * next turn, BEFORE delivering anything new into JavaScript (sec.2.1). */
bool pocketjs_guest_jobs_pending(const pocketjs_guest_t *guest);

/** What the current LOGICAL drain has cost: microseconds spent inside
 * vm_sched_drain() and jobs completed, summed over the drain the budget cut
 * and every continuation of it, both cleared when the queue empties. The host
 * runaway guard (docs/vm-L1-design.md sec.5.2) is a predicate on these; it
 * cannot be a turn count, because a turn ends for reasons -- the backstop, a
 * contended machine shortening the wall-clock budget -- that say nothing about
 * how much work the guest asked for. `*us` is 0 whenever the budget runs in
 * count mode (limit_us <= 0), where the clock is deliberately never read. */
void pocketjs_guest_drain_total(const pocketjs_guest_t *guest, int64_t *us,
                                uint64_t *jobs);

/** Continue the drain the budget cut. Same queue, same order, no frame() and
 * no new host events in between: this is the rest of one logical drain. */
esp_err_t pocketjs_guest_continue(pocketjs_guest_t *guest);

/** Install the host's interrupt predicate (sec.5.3). QuickJS has ONE handler
 * slot and three callers used to overwrite one another; the registration now
 * lives in the guest and the host swaps only the predicate. NULL restores the
 * guest's own epoch-based handler. Returns 1 to interrupt, like QuickJS. */
void pocketjs_guest_set_watchdog(pocketjs_guest_t *guest, int (*fn)(void *),
                                 void *opaque);

/** Ask the current or next JavaScript turn to stop through QuickJS's handler.
 * This is the only guest API that may be called outside the owner task. */
void pocketjs_guest_interrupt(pocketjs_guest_t *guest);

esp_err_t pocketjs_guest_stats(pocketjs_guest_t *guest,
                               pocketjs_guest_stats_t *out_stats);

void pocketjs_guest_destroy(pocketjs_guest_t *guest);

/* VM_PROBE (docs/quickjs-freertos-vm-spec.md sec.5). __has_include, not a bare
 * include: this header is also compiled on the host by tools/vmtest, where
 * there is no sdkconfig.h. Absent config == probe off, the shipping default. */
#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#else
#include "sdkconfig.h"
#endif

#ifdef CONFIG_POCKET_VM_PROBE
/** The split sec.5 asks for that main/pocket/vmprobe.c cannot take from
 * outside: pocketjs_ui_turn() is frame() + drain + the UI core's tick and
 * draw, and only guest.c sees the boundary between the first two. Both
 * counters accumulate over every pocketjs_guest_frame() since the previous
 * take (one per frame today) and are zeroed by it, so the caller reads
 * "this frame's". Two clock reads per frame, not per job. */
void pocketjs_guest_vmprobe_take(uint32_t *call_us, uint32_t *drain_us);

/** vm-l1-tuning: raw `ran` from every vm_sched_drain() call (drain_jobs(),
 * the choke point both pocketjs_guest_frame() and pocketjs_guest_continue()
 * share) since the last take, up to `cap` entries -- oldest-first, FIFO by
 * insertion. Returns how many were written into `out` (NULL to just drain and
 * count); `*dropped` is how many more happened than the internal buffer
 * (128) could hold. Resets both counters. This is the ONLY way to see a
 * continuation call's job count: it never reaches vmprobe_frame_sample(),
 * whose per-turn "jobs" sample only covers calls a frame() tick made and
 * folds an unreported continuation into whichever later tick does call it. */
unsigned pocketjs_guest_vmprobe_drain_calls(uint16_t *out, unsigned cap,
                                            unsigned *dropped);
#endif

#ifdef __cplusplus
}
#endif
