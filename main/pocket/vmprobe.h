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

// The fixed contention conditions sec.5's completion condition asks for
// ("UI・音声・通信との競合条件、入力データ、反復数を固定"). A bit mask rather
// than a list of named conditions, so base / ui / audio / wifi / all and every
// other combination are one mechanism: the host selects one with a single USB
// byte ('P' + mask, main.c's usb_stroke) before starting a workload, and
// app_session.c hands the mask to apps/vmprobe/condition.js, evaluated after
// the workload's own source. What each bit starts is in that file and in
// apps/vmprobe/README.md.
#define VMPROBE_COND_UI    1u
#define VMPROBE_COND_AUDIO 2u
#define VMPROBE_COND_WIFI  4u
#define VMPROBE_COND_ALL   7u

// Set from the input task, read on the ui task when a session starts. Sticky:
// it stays until the host sends another letter, so a capture script sets the
// condition once and runs all six workloads under it.
void vmprobe_condition_set(unsigned mask);
unsigned vmprobe_condition(void);

// Logged at every guest start (app_start_test(), right after the guest
// exists): engine revision, compiler, optimization level and the struct
// sizes sec.5 asks for. All of it is a build-time fact; it repeats per
// session so tools/vm_l0_capture.py sees one at the head of each workload.
// Also rebases the window and the job counters for the new session.
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

// G1's device side (docs/vm-L2-design.md sec.1.3: "host is vmrun/
// stack_probe.sh; the device is confirmed with uxTaskGetStackHighWaterMark").
// Call with the JS call depth the caller has JUST reached (i.e. from inside
// the deepest active JS frame), right next to vmprobe_frame_sample's own
// stack_hw_min sample above. uxTaskGetStackHighWaterMark reports a
// monotonically NON-INCREASING low-water mark since the ui task started, so
// sampling it partway through a single, ever-deepening recursive probe (the
// shape apps/vmprobe/deep_recursion.js already uses to find the JS-visible
// stack limit at session start, before any frame() has run) attributes each
// sample to the depth the caller was actually at: nothing deeper has
// happened yet in that task's life. Wired from JS through
// __vmprobe_stack_sample() in main/ui/jsconsole.c (also CONFIG_POCKET_VM_PROBE-
// only), not from quickjs.c -- this is a probe workload calling out, not an
// interpreter checkpoint, so it costs nothing on the interpreter's fast path.
void vmprobe_depth_stack_sample(uint32_t depth);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_POCKET_VM_PROBE
