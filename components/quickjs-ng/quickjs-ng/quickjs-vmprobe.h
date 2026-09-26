// VM_PROBE declarations (docs/vm/quickjs-freertos-vm-spec.md sec.5, L0).
//
// This file is NOT part of the upstream quickjs-ng import -- it is new, added
// alongside the vendored sources so main/pocket/vmprobe.c can reach the few
// counters and accessors quickjs.c defines under #ifdef CONFIG_POCKET_VM_PROBE
// (see the "VM_PROBE" blocks in quickjs.c). Kept separate from quickjs.h so
// re-vendoring quickjs.h from upstream never has to route around it.
//
// Every declaration here is guarded the same way its quickjs.c definition is:
// with the config off, this header declares nothing and costs nothing.
#pragma once

#include <stddef.h>
#include <stdint.h>
// CONFIG_POCKET_VM_PROBE lives here. ESP-IDF puts the build's config/
// directory on every component's include path but does NOT force-include
// this header into every translation unit, so anyone gating on the config
// macro has to pull it in explicitly -- this header does, once, so its own
// includers do not each have to know that.
//
// __has_include, not a bare #include: quickjs.c includes this header, and
// host builds compile quickjs.c with no IDF config dir on the path
// (tools/build_pocket_text_test.sh passes only -I <quickjs dir>). A bare
// include broke that build outright. No sdkconfig.h means no
// CONFIG_POCKET_VM_PROBE, which is the shipping default anyway.
#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#else
#include "sdkconfig.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_POCKET_VM_PROBE

// sizeof(JSStackFrame) / sizeof(JSVarRef): private structs, not otherwise
// reachable outside quickjs.c.
size_t qjs_vmprobe_sizeof_stack_frame(void);
size_t qjs_vmprobe_sizeof_var_ref(void);

// Current job_list depth (JS_EnqueueJob minus JS_ExecutePendingJob).
size_t qjs_vmprobe_job_queue_len_get(void);
// Peak depth since the last call, which also rebases the peak to "now" --
// call this once per sample window, not per frame and once elsewhere too.
size_t qjs_vmprobe_job_queue_peak_take(void);
// Cumulative jobs executed by JS_ExecutePendingJob over the runtime's life;
// a caller diffs two reads to get "jobs executed this window".
uint64_t qjs_vmprobe_jobs_executed_get(void);

#endif // CONFIG_POCKET_VM_PROBE

// F-line measurement (CONFIG_POCKET_VM_FLOORPROBE, docs/vm/builtin-floor-plan.md
// sec.14): counters of the flash-atom (F1) and lazy-builtin (F2) paths,
// read and cleared by JS_TakeFloorProbe (quickjs.h). The counters and macros
// live here rather than in quickjs.c so that quickjs.c gains no line: its
// assert()s embed __LINE__, and a shifted line changes the code of the
// option-off build (the same reason JS_VMStackBlocks sits at the end of it).
// quickjs.c calls FP_INC/FP_ALL on lines that already existed.
#ifdef CONFIG_POCKET_VM_FLOORPROBE
#include "quickjs.h"
static __attribute__((unused)) JSFloorProbe js_floor_probe;
#define FP_INC(rt, field) ((void)(rt), js_floor_probe.field++)
#define FP_ALL(p, enum_only) do { \
        if (js_floor_probe.lazy_all < 8) { \
            js_floor_probe.all_class[js_floor_probe.lazy_all] = (uint16_t)(p)->class_id; \
            js_floor_probe.all_enum_only[js_floor_probe.lazy_all] = (enum_only); \
        } \
        js_floor_probe.lazy_all++; \
    } while (0)
#else
#define FP_INC(rt, field) ((void)0)
#define FP_ALL(p, enum_only) ((void)0)
#endif

#ifdef __cplusplus
}
#endif
