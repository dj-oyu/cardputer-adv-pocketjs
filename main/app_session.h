#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
esp_err_t app_start(void);
// Run a source the caller owns; the bytes must outlive the start, and only the
// start -- the guest parses straight out of them and keeps nothing afterwards.
//
// `prelude` is evaluated first and in the same realm, so its top-level
// declarations are visible to `source` while the two keep their own line
// numbering in an error. NULL when there is none.
esp_err_t app_start_source(const char *prelude, size_t prelude_length,
                           const char *source, size_t length);
esp_err_t app_start_test(char test);
#if defined(CONFIG_POCKET_VM_RELOC) && defined(CONFIG_POCKET_VM_YIELD)
/* L3a: every app started from now on moves its frame segments at every park,
 * and logs VM_RELOC when it stops. Only linked in CONFIG_POCKET_VM_RELOC. */
void app_vm_reloc_request(void);
#endif
void app_vm_back_selftest(void); /* Only linked in CONFIG_POCKET_VM_SELFTEST. */
/* The F-line microbenchmark's USB key ('(', CONFIG_POCKET_VM_FLOORPROBE).
 * A macro rather than an #ifdef block in main.c's usb_stroke(): a block there
 * adds lines, and app_main's ESP_ERROR_CHECKs below it embed __LINE__, so the
 * option-off build would stop being byte-identical. Off, it folds to 0. */
#ifdef CONFIG_POCKET_VM_FLOORPROBE
#define APP_FLOOR_BENCH_KEY(c) ((c) == '(')
#else
#define APP_FLOOR_BENCH_KEY(c) 0
#endif

// docs/api/common-api.md 3.1: a session the HOME SCREEN owns, running over the
// background rather than instead of it. It gets a region-scoped Kasane APP
// endpoint; the shell supplies the live native scene as its backdrop. The
// guest heap is sized for what is left while a scene is drawing rather than
// for an empty machine.
//
// The bytes must outlive the start, as with app_start_source().
//
// THIS IS A CEILING, NOT A RESERVATION, and getting that wrong cost a board
// test. guest.c calls JS_SetMemoryLimit() with it and THEN JS_NewContext(),
// so the cap is in force while QuickJS builds its intrinsics: set below the
// cost of an empty realm, it does not produce a small guest, it produces
// ESP_ERR_NO_MEM out of guest creation on a machine with 278,820 bytes free.
// 48 KiB did exactly that on 2026-09-09.
//
// Because it is a ceiling, a generous value costs nothing: QuickJS allocates
// what it uses and no more. So this is set clear of every recorded figure --
// 85,602 bytes for hello/main.js before the pocket surfaces existed, js=86,233
// measured on the board for a running app -- rather than tuned close to one.
// It was 128 KiB and that was too close, which took a board run to find: the
// player asked for pocket.fs on its first keypress, the lazy namespace could
// not be built, and the error came back as "no memory to build this namespace"
// from inside a key listener -- nowhere near the allocation that was actually
// short. A ceiling set near the cost is a ceiling that turns growth in an app
// into a failure in an unrelated surface.
//
// So it is the foreground value now. An overlay installs seven surfaces instead
// of fifteen and will not reach it; what actually defends the machine is the
// free-heap check below, which measures instead of predicting.
//
// What actually defends the machine is not this number. It is the free-heap
// check ui/overlay.c takes AFTER the guest is up, which measures what was
// spent instead of predicting it.
#define OVERLAY_GUEST_HEAP (160*1024)
esp_err_t app_start_overlay(const char *source, size_t length);
// One turn of an overlay session. Guest JavaScript is advanced and its Kasane
// transaction is closed here. Presentation remains shell-owned so it can load
// the native scene into each strip before composing the APP layer.
esp_err_t app_overlay_tick(void);
void app_force_redraw(void);
// The same, for an owner that composites over the guest's strips and knows
// which 8-row bands it touched (bit b is rows 8b..8b+7, bit 16 the last seven).
// An empty set is a no-op.
void app_force_redraw_bands(uint32_t bands);
#ifdef KASANE_P2_REPAIR_PROBE
// USB diagnostic only: fail the fourth send of one forced full APP repaint.
void app_p2_request_repair_probe(void);
// USB diagnostic only: fail the fourth send of the next natural APP PATCH.
void app_p2_request_patch_repair_probe(void);
#endif
esp_err_t app_tick(uint32_t buttons);
// The display period the shell paces a RUNNING guest at (main.c's ui_task),
// and the ceiling on how often a continuation turn reaches the panel
// (app_session.c). One constant because the two are the same decision seen
// from both ends: how often a person may be shown a new picture.
#define VM_DISPLAY_PERIOD_MS 33
// True when the turn app_tick() just ran was a CONTINUATION that returned with
// the job queue still non-empty -- a turn with no frame() and, after the
// throttle in app_session.c, usually no transfer either. main.c asks because
// such a turn has nothing to present and so must not be charged a display
// period before the next one (docs/vm/vm-L1-report.md sec.2.4).
bool app_turn_continued(void);
void app_stop(void);
void app_request_stop(void);
// Swap the predicate QuickJS's single interrupt slot answers with
// (docs/vm/vm-L1-design.md sec.5.3). The registration itself lives in the guest;
// three callers used to overwrite one another in that slot, which is why the
// guest's own handler had been dead since the first of them. NULL restores the
// session watchdog (stop requested, or the 250 ms deadline).
void app_vm_watchdog(int (*fn)(void *), void *opaque);
// Close any parked bytecode chain before entering a shutdown hook.
void app_vm_prepare_stop(void);
void app_report(void);
// The last exception a Playground run reported, or "" when it ran clean.
const char *app_error(void);
