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

// F3b/F3c (POCKET_VM_LAZY_INTRINSICS, docs/vm/builtin-floor-plan.md
// sec.17-18): typed-array, Map/Set, WeakRef and DOMException constructors
// made on first use. The hooks are macros placed on lines of quickjs.c; off
// (only in tools/vmtest/floor/gen_rom_atoms.sh, see quickjs.c) each expands
// to nothing or to a constant the compiler folds. The functions they name are
// at the end of quickjs.c.
#ifdef POCKET_VM_LAZY_INTRINSICS
#define JS_DEF_POCKET_LAZY_CLASS 12   /* after JS_DEF_PROP_BOOL; quickjs.c only */
#define LAZY_CLASS_DECLS \
    static JSValue js_lazy_class_ctor(JSContext *ctx, int class_id); \
    static int js_lazy_class_ensure(JSContext *ctx, int class_id); \
    static int js_lazy_ta_register(JSContext *ctx); \
    static int js_lazy_register(JSContext *ctx, int group);
#define LAZY_CLASS_CASE \
    case JS_DEF_POCKET_LAZY_CLASS: val = js_lazy_class_ctor(ctx, e->magic); break;
/* true = the class's prototype was pending and making it failed */
#define LAZY_CLASS_MISSING(ctx, id) \
    (unlikely(JS_IsNull((ctx)->class_proto[id])) && js_lazy_class_ensure(ctx, id) < 0)
/* ta_base: %TypedArray%. lazy_groups: which LAZY_G_* this context registered
   lazily (a group it did not is upstream's, JS_NULL prototype and all). */
#define LAZY_CTX_FIELDS JSValue ta_base; uint8_t lazy_groups;
#define LAZY_CTX_INIT ctx->ta_base = JS_NULL; ctx->lazy_groups = 0;
#define LAZY_CTX_MARK JS_MarkValue(rt, ctx->ta_base, mark_func);
#define LAZY_CTX_FREE JS_FreeValue(ctx, ctx->ta_base);
#define LAZY_TA_REGISTER(ctx) return js_lazy_ta_register(ctx);
/* F3c: the same for Map/Set, WeakRef/FinalizationRegistry and DOMException,
   placed at the top of their JS_AddIntrinsic* in place of the rest of it. */
#define LAZY_G_TA 0
#define LAZY_G_MAPSET 1
#define LAZY_G_WEAKREF 2
#define LAZY_G_DOMEX 3
#define LAZY_GROUP_REGISTER(ctx, group) return js_lazy_register(ctx, group);
#else
#define LAZY_GROUP_REGISTER(ctx, group)
#define LAZY_CLASS_DECLS
#define LAZY_CLASS_CASE
#define LAZY_CLASS_MISSING(ctx, id) 0
#define LAZY_CTX_FIELDS
#define LAZY_CTX_INIT
#define LAZY_CTX_MARK
#define LAZY_CTX_FREE
#define LAZY_TA_REGISTER(ctx)
#endif

#ifdef __cplusplus
}
#endif
