// L2 VM harness hooks: forced yield and the G5 gap recorder. See quickjs-vm.h
// for what this is and why it is not inside quickjs.c.
//
// Nothing here allocates on the yield path. The state block is allocated once
// by js_vm_arm() (in quickjs.c, through js_vm_state_alloc); the recorder only ever
// touches fixed arrays and atom refcounts. JS_DupAtomRT / JS_FreeAtomRT do not
// allocate -- an atom's storage already exists while anything references it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs-vm.h"
#include "quickjs-vmstack.h"

// ---------------------------------------------------------------- segment stack

int vmtest_vmstack_configure(JSRuntime *rt, size_t seg_size, unsigned cache_max)
{
    JSVMStack *st = js_vm_stack_get(rt);
    if (!st)
        return -1;
    return js_vm_stack_configure(st, seg_size, cache_max);
}

int vmtest_vmstack_set_budget(JSRuntime *rt, size_t bytes)
{
    JSVMStack *st = js_vm_stack_get(rt);
    if (!st)
        return -1;
    st->budget = bytes;
    return 0;
}

void vmtest_vmstack_report(JSRuntime *rt, void *out)
{
    FILE *f = out;
    JSVMStack *st = js_vm_stack_get(rt);
    if (!st)
        return;
#ifdef JS_VM_STACK_STATS
    // resident_max: bytes the segments held at their peak, header and
    // alignment slack included -- what the guest heap actually gave up, as
    // opposed to live_max, the frame bytes that were in use at the peak.
    // budget / budget_hits / seg_refused: D10 -- the limit in force at the
    // end, how many pushes it refused (each one a RangeError the C-stack
    // test did not get to raise first), and how many pushes the RUNTIME
    // refused (no memory for a segment: each one an InternalError). A run in
    // which the budget did its job has seg_refused=0; budget_probe.sh reads
    // both. The harness allocator's own "fails=" counts only injected
    // --fail-alloc failures, not JS_SetMemoryLimit refusals, which never
    // reach it.
    // flat=: whether this binary keeps JS-to-JS calls in one C activation
    // (CONFIG_POCKET_VM_FLATCALLS). budget_probe.sh reads it to know which
    // guard is expected to answer under the shipped limits: with C recursion
    // the C-stack guard fires first (budget_hits=0), flat only the budget can.
    size_t overhead = sizeof(JSVMSeg) + (JS_VM_SEG_ALIGN - 1);
#ifdef CONFIG_POCKET_VM_FLATCALLS
    const int flat = 1;
#else
    const int flat = 0;
#endif
    // The (unsigned) casts are not decoration: depth_max and seg_live_max are
    // uint32_t, which is unsigned int on the x86-64 host but long unsigned int
    // on xtensa, so a bare %u passed every host build and failed -Werror=format
    // the first time a CONFIG_POCKET_VM_PROBE firmware compiled this file.
    fprintf(f, "#info vmstack flat=%d seg_size=%zu align=%d frame_hdr=%zu pushes=%llu depth_max=%u "
               "live_max=%zu frame_max=%zu seg_live_max=%u seg_mallocs=%llu seg_frees=%llu "
               "seg_reuses=%llu dedicated=%llu fallbacks=%llu resident_max~=%zu "
               "budget=%zu budget_hits=%llu seg_refused=%llu\n",
            flat, st->seg_size, JS_VM_FRAME_ALIGN, sizeof(JSVMSeg),
            (unsigned long long)st->pushes, (unsigned)st->depth_max, st->live_bytes_max, st->frame_max,
            (unsigned)st->seg_live_max, (unsigned long long)st->seg_mallocs,
            (unsigned long long)st->seg_frees, (unsigned long long)st->seg_reuses,
            (unsigned long long)st->dedicated, (unsigned long long)st->fallbacks,
            (size_t)st->seg_live_max * (st->seg_size + overhead),
            st->budget, (unsigned long long)st->budget_hits,
            (unsigned long long)st->seg_refused);
#else
#ifdef CONFIG_POCKET_VM_FLATCALLS
    fprintf(f, "#info vmstack flat=1 seg_size=%zu budget=%zu (no stats in this build)\n",
            st->seg_size, st->budget);
#else
    fprintf(f, "#info vmstack flat=0 seg_size=%zu budget=%zu (no stats in this build)\n",
            st->seg_size, st->budget);
#endif
#endif
}

// ---------------------------------------------------------------- gaps

static void gap_release(JSRuntime *rt, JSVMGap *g)
{
    JS_FreeAtomRT(rt, g->start_func);
    JS_FreeAtomRT(rt, g->end_func);
    g->start_func = g->end_func = JS_ATOM_NULL;
}

// Closes the gap that started at vm->gap_start_* and ends now at (kind, func).
// The interval is kept only if it makes the top list; the atoms it carries are
// released otherwise. `func` is borrowed.
static void gap_close(JSRuntime *rt, JSVMState *vm, uint64_t now,
                      JSVMOpportunity kind, JSAtom func)
{
    uint64_t ns = now - vm->gap_start_ns;
    vm->gaps++;
    vm->gap_ns_total += ns;
    // The list is short and full most of the time; the common case is one
    // comparison against the smallest kept entry.
    if (vm->top_n == JS_VM_TOP_GAPS && ns <= vm->top[JS_VM_TOP_GAPS - 1].ns) {
        JS_FreeAtomRT(rt, vm->gap_start_func);
        vm->gap_start_func = JS_ATOM_NULL;
        return;
    }
    int i = vm->top_n;
    if (i == JS_VM_TOP_GAPS) {
        i--;
        gap_release(rt, &vm->top[i]);
    }
    while (i > 0 && vm->top[i - 1].ns < ns) {
        vm->top[i] = vm->top[i - 1];
        i--;
    }
    vm->top[i].ns = ns;
    vm->top[i].start_kind = vm->gap_start_kind;
    vm->top[i].end_kind = kind;
    vm->top[i].start_func = vm->gap_start_func;   // ownership moves
    vm->top[i].end_func = JS_DupAtomRT(rt, func);
    vm->gap_start_func = JS_ATOM_NULL;
    if (vm->top_n < JS_VM_TOP_GAPS)
        vm->top_n++;
}

static void gap_open(JSRuntime *rt, JSVMState *vm, uint64_t now,
                     JSVMOpportunity kind, JSAtom func)
{
    JS_FreeAtomRT(rt, vm->gap_start_func);
    vm->gap_start_ns = now;
    vm->gap_start_kind = kind;
    vm->gap_start_func = JS_DupAtomRT(rt, func);
}

// ---------------------------------------------------------------- hooks

int js_vm_safepoint(JSRuntime *rt, JSVMState *vm, JSAtom func)
{
    vm->safepoints++;
    if (vm->gap_on && vm->in_js) {
        uint64_t now = vm->clock();
        gap_close(rt, vm, now, JS_VM_OPP_SAFEPOINT, func);
        gap_open(rt, vm, now, JS_VM_OPP_SAFEPOINT, func);
    }
    if (vm->force_yield) {
        vm->stops++;
        return 1;
    }
    return 0;
}

void js_vm_enter(JSRuntime *rt, JSVMState *vm)
{
    // A nested prologue poll never gets here (quickjs.c checks for no frame),
    // but JS_CallConstructorInternal polls once itself and then again inside
    // JS_CallInternal, both with no frame: the second must be a no-op, not an
    // opportunity, or it would split a gap where nothing can stop.
    if (vm->in_js)
        return;
    vm->enters++;
    vm->in_js = 1;
    if (vm->gap_on)
        gap_open(rt, vm, vm->clock(), JS_VM_OPP_ENTER, JS_ATOM_NULL);
}

void js_vm_leave(JSRuntime *rt, JSVMState *vm, JSAtom func)
{
    if (!vm->in_js)
        return;
    vm->leaves++;
    vm->in_js = 0;
    if (vm->gap_on) {
        gap_close(rt, vm, vm->clock(), JS_VM_OPP_LEAVE, func);
        JS_FreeAtomRT(rt, vm->gap_start_func);
        vm->gap_start_func = JS_ATOM_NULL;
    }
}

JSVMState *js_vm_state_alloc(void)
{
    return calloc(1, sizeof(JSVMState));
}

void js_vm_state_free(JSRuntime *rt, JSVMState *vm)
{
    for (int i = 0; i < vm->top_n; i++)
        gap_release(rt, &vm->top[i]);
    vm->top_n = 0;
    JS_FreeAtomRT(rt, vm->gap_start_func);
    vm->gap_start_func = JS_ATOM_NULL;
    free(vm);
}

// ---------------------------------------------------------------- harness

void vmtest_vm_set_force_yield(JSRuntime *rt, int on)
{
    JSVMState *vm = js_vm_arm(rt, 1);
    vm->force_yield = on ? 1 : 0;
}

void vmtest_vm_set_gap_clock(JSRuntime *rt, JSVMClock clock, int on)
{
    JSVMState *vm = js_vm_arm(rt, 1);
    vm->clock = clock;
    vm->gap_on = (on && clock) ? 1 : 0;
}

static const char *opp_name(JSVMOpportunity k)
{
    switch (k) {
    case JS_VM_OPP_SAFEPOINT: return "safepoint";
    case JS_VM_OPP_ENTER: return "enter";
    case JS_VM_OPP_LEAVE: return "leave";
    default: return "none";
    }
}

// Writes "<kind>:<func>" -- the function is the one running at that end of
// the gap, "-" when it was not bytecode (an enter, or a native frame).
static void print_end(FILE *f, JSContext *ctx, JSVMOpportunity k, JSAtom func)
{
    fprintf(f, "%s:", opp_name(k));
    if (func == JS_ATOM_NULL) {
        fputs("-", f);
        return;
    }
    const char *s = JS_AtomToCString(ctx, func);
    fputs(s && *s ? s : "<anon>", f);
    JS_FreeCString(ctx, s);
}

void vmtest_vm_report(JSRuntime *rt, JSContext *ctx, void *out)
{
    FILE *f = out;
    JSVMState *vm = js_vm_state(rt);
    if (!vm)
        return;
    fprintf(f, "#info vm force_yield=%d safepoints=%llu stops=%llu enters=%llu leaves=%llu\n",
            vm->force_yield, (unsigned long long)vm->safepoints,
            (unsigned long long)vm->stops, (unsigned long long)vm->enters,
            (unsigned long long)vm->leaves);
    if (!vm->gap_on)
        return;
    // G5 is the maximum. The total and count are context for reading it, not
    // a substitute (design sec.1.3: a mean does not satisfy the condition).
    fprintf(f, "#info g5 max_ns=%llu gaps=%llu total_ns=%llu",
            (unsigned long long)(vm->top_n ? vm->top[0].ns : 0),
            (unsigned long long)vm->gaps, (unsigned long long)vm->gap_ns_total);
    if (vm->top_n) {
        fputs(" start=", f);
        print_end(f, ctx, vm->top[0].start_kind, vm->top[0].start_func);
        fputs(" end=", f);
        print_end(f, ctx, vm->top[0].end_kind, vm->top[0].end_func);
    }
    fputc('\n', f);
    for (int i = 0; i < vm->top_n; i++) {
        fprintf(f, "#info g5 top[%d] ns=%llu start=", i, (unsigned long long)vm->top[i].ns);
        print_end(f, ctx, vm->top[i].start_kind, vm->top[i].start_func);
        fputs(" end=", f);
        print_end(f, ctx, vm->top[i].end_kind, vm->top[i].end_func);
        fputc('\n', f);
    }
}
