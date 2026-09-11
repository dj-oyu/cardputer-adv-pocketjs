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
// session so a capture sees one at the head of each app run. Also rebases
// the window and the job counters for the new session.
void vmprobe_static_report(void);

// Called once per app_tick() frame, after the JS turn (frame() call plus
// whatever job draining pocketjs_ui_turn does around it) has been timed.
// Collects the frame's raw samples and writes one "VMPROBE WINDOW ..." block
// when the window closes -- never per frame, so the logging itself cannot
// distort the timing it reports (sec.5's own requirement).
//
// The job counters come from the vendored quickjs.c (see quickjs-vmprobe.h)
// and increment on every JS_EnqueueJob / JS_ExecutePendingJob in the whole
// firmware, not just at this call site, so they are exact regardless of who
// drains the queue. turn_us is the whole of pocketjs_ui_turn(); frame() time
// and drain time are taken separately from the vendored guest.c
// (pocketjs_guest_vmprobe_take), so the UI core's tick and draw are what is
// left over rather than a guessed split.
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
