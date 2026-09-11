// L0 measurement probes (docs/quickjs-freertos-vm-spec.md sec.5). Every
// declaration here is only meaningful, and only compiled in, with
// CONFIG_POCKET_VM_PROBE on -- see main/Kconfig.projbuild. Call sites in
// app_session.c and pocket_api.c include this unconditionally and wrap the
// calls in #ifdef CONFIG_POCKET_VM_PROBE, so this header has to make the
// config macro visible itself.
#pragma once

// __has_include: pocket_api.c is also compiled on the host
// (tools/build_pocket_capture_test.sh, build_pocket_random_test.sh,
// test_lazy_namespace.c), where tools/hostshim has no sdkconfig.h. A bare
// include broke all three; absent config == probes off, the shipping default.
#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#else
#include "sdkconfig.h"
#endif

#ifdef CONFIG_POCKET_VM_PROBE

#include "pocketjs/guest.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Logged at every guest start (app_start_test(), right after the guest
// exists): engine revision, compiler, optimization level and the struct
// sizes sec.5 asks for. All of it is a build-time fact; it repeats per
// session so tools/vm_l0_capture.py sees one at the head of each workload.
// Also rebases the window and the job counters for the new session.
void vmprobe_static_report(void);

// Called once per app_tick() frame, after the JS turn (frame() call plus
// whatever job draining pocketjs_ui_turn does around it) has been timed.
// Accumulates into a ~1s rolling window and logs "VMPROBE WINDOW ..." when
// the window elapses -- never per frame, so the logging itself cannot
// distort the timing it reports (sec.5's own requirement).
//
// jobs_this_frame and queue_peak_this_frame come from counters inside the
// vendored quickjs.c (see quickjs-vmprobe.h) that increment on every
// JS_EnqueueJob / JS_ExecutePendingJob call in the whole firmware, not just
// this call site -- so they are exact regardless of which component (this
// app's own loop, a Promise microtask, pocketjs_ui_qjs internally) is the
// one draining the queue. What this file cannot isolate, because
// pocketjs_ui_qjs's internal call to frame()+drain is opaque (a prebuilt
// component, not vendored here), is drain time held apart from frame()'s
// own time: turn_us below is their sum, honestly reported as one number
// rather than a guessed split.
void vmprobe_frame_sample(pocketjs_guest_t *guest, int64_t turn_us);

// Called from pocket_api.c's pump, at the moment a completion's resolve/
// reject actually runs, with (now - the timestamp pocket_api_complete()
// recorded). This stops at the resolve/reject call, not at the JS handler:
// reactions run later, in drain_jobs() after frame() (docs/vm-ledger/03).
void vmprobe_completion_sample(int64_t latency_us);

// Called from app_stop(): resets the rolling window so one session's tail
// does not blend into the next session's head in a captured record.
void vmprobe_session_reset(void);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_POCKET_VM_PROBE
