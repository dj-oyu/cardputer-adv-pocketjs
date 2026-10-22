static int fa_function_allocation_active;
static unsigned fa_force_shift;
static unsigned long fa_function_raw_attempts;
/* Include the real private frame type and the actual segment allocator. */
#include "quickjs.c"
#include "bytecode_alignment.h"

#ifdef FA_EXPECT_ALIGN8
_Static_assert(_Alignof(JSStackFrame) == 8, "official 32-bit ABI requires alignment 8");
_Static_assert(sizeof(JSStackFrame) == 48, "32-bit frame body must stay 48 bytes");
_Static_assert(sizeof(JSVMLink) == 8, "32-bit prefix includes 4 bytes of padding");
#endif

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    JSRuntime *rt = JS_NewRuntime();
    assert(rt != NULL);
    JSVMStack st;
    void *blocks[64];
    size_t charges[64];
    size_t total = 0;
    js_vm_stack_init(&st);
    assert(js_vm_stack_configure(&st, 256, 2) == 0);
    for (unsigned i = 0; i < countof(blocks); i++) {
        /* Odd pointer tails used to misalign later blocks even if the
           first prefix happened to be patched in isolation. */
        size_t slots = 1 + i % 3, refs = i % 4;
        size_t size = JS_VM_FRAME_PREFIX + sizeof(JSStackFrame) +
                      slots * sizeof(JSValue) + refs * sizeof(JSVarRef *);
        charges[i] = js_vm_stack_round(size);
        blocks[i] = js_vm_stack_push(rt, &st, size);
        assert(blocks[i] != NULL);
        JSVMLink *link = blocks[i];
        JSStackFrame *sf = (JSStackFrame *)(link + 1);
        JSValue *values = (JSValue *)(sf + 1);
        JSVarRef **pointers = (JSVarRef **)(values + slots);
        assert((uintptr_t)link % _Alignof(JSVMLink) == 0);
        assert((uintptr_t)sf % _Alignof(JSStackFrame) == 0);
        assert((uintptr_t)values % _Alignof(JSValue) == 0);
        assert((uintptr_t)pointers % _Alignof(JSVarRef *) == 0);
        assert((uint8_t *)sf - (uint8_t *)link == JS_VM_FRAME_PREFIX);
        assert(((JSVMLink *)sf) - 1 == link);
        memset(sf, 0, sizeof(*sf));
        sf->cur_func = JS_UNDEFINED;
        sf->arg_buf = values;
        sf->var_refs = pointers;
        link->caller_sp = values + slots;
        for (size_t j = 0; j < slots; j++) values[j] = JS_MKVAL(JS_TAG_INT, i + j);
        for (size_t j = 0; j < refs; j++) pointers[j] = NULL;
        assert((((JSVMLink *)sf) - 1)->caller_sp == values + slots);
        total += charges[i];
        assert(st.used == total);
    }
    for (unsigned i = countof(blocks); i-- > 0;) {
        assert(js_vm_stack_holds(&st, blocks[i]));
        js_vm_stack_pop(rt, &st, blocks[i]);
        total -= charges[i];
        assert(st.used == total);
    }
    assert(total == 0 && st.depth == 0);
    js_vm_stack_free(rt, &st);

    js_vm_stack_init(&st);
    size_t size = JS_VM_FRAME_PREFIX + sizeof(JSStackFrame) + sizeof(JSValue) + sizeof(JSVarRef *);
    size_t rounded = js_vm_stack_round(size);
    st.budget = rounded - 1;
    assert(js_vm_stack_over_budget(&st, size));
    assert(st.used == 0 && st.cur == NULL);
    st.budget = rounded;
    assert(!js_vm_stack_over_budget(&st, size));
    void *block = js_vm_stack_push(rt, &st, size);
    assert(block != NULL && st.used == rounded);
    assert(js_vm_stack_over_budget(&st, 1));
    js_vm_stack_pop(rt, &st, block);
    assert(st.used == 0 && st.depth == 0);
    js_vm_stack_free(rt, &st);

    /* The padding is included in dedicated frames and refused pushes too. */
    js_vm_stack_init(&st);
    size = st.seg_max + JS_VM_FRAME_ALIGN;
    block = js_vm_stack_push(rt, &st, size);
    assert(block != NULL && st.used == js_vm_stack_round(size));
    assert(st.dedicated == 1);
    js_vm_stack_pop(rt, &st, block);
    assert(st.cur == NULL && st.used == 0 && st.depth == 0);
    js_vm_stack_free(rt, &st);

    js_vm_stack_init(&st);
    JS_SetMemoryLimit(rt, 1);
    assert(js_vm_stack_push(rt, &st, size) == NULL);
    assert(st.used == 0 && st.depth == 0 && st.cur == NULL && st.seg_refused == 1);
    JS_SetMemoryLimit(rt, 0);
    js_vm_stack_free(rt, &st);
    printf("frame alignment PASS ptr=%zu value=%zu frame=%zu frame_align=%zu link=%zu block_align=%d odd_charge=%zu\n",
           sizeof(void *), sizeof(JSValue), sizeof(JSStackFrame), _Alignof(JSStackFrame),
           sizeof(JSVMLink), JS_VM_FRAME_ALIGN, rounded);
    for (fa_force_shift=0;fa_force_shift<=4;fa_force_shift+=4) {
        fa_shift_gc_test();
        fa_bc_test(rt);
    }
    JS_FreeRuntime(rt);
    return 0;
}
