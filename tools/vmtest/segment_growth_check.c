// D42 diagnostic configuration must be atomic and preserve cache policy.
#undef NDEBUG
#include "cutils.h"
#include "quickjs.h"
#include "quickjs-vmstack.h"

int main(void) {
    JSVMStack stack;
    js_vm_stack_init(&stack);
    stack.cache_max = 19;
    assert(js_vm_stack_configure_growth(&stack, 256, 2048) == 0);
    assert(stack.seg_first == 256 && stack.seg_max == 2048 && stack.cache_max == 19);
    assert(js_vm_seg_want(&stack, 1) == 256);
    assert(js_vm_seg_want(&stack, 3) == 768);
    assert(js_vm_seg_want(&stack, UINT32_MAX) == 2048);
    const size_t bad[][2] = {{0, 4096}, {17, 4096}, {512, 256},
                             {512, 4000}, {16, SIZE_MAX}};
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        assert(js_vm_stack_configure_growth(&stack, bad[i][0], bad[i][1]) == -1);
        assert(stack.seg_first == 256 && stack.seg_max == 2048 && stack.cache_max == 19);
    }
    JSVMSeg segment = {0};
    stack.cur = &segment;
    assert(js_vm_stack_configure_growth(&stack, 512, 4096) == -1);
    stack.cur = NULL;
    stack.cache = &segment;
    assert(js_vm_stack_configure_growth(&stack, 512, 4096) == -1);
    stack.cache = NULL;
    assert(stack.seg_first == 256 && stack.seg_max == 2048 && stack.cache_max == 19);
    // The legacy fixed-size API still rounds and changes cache policy.
    assert(js_vm_stack_configure(&stack, 257, 1) == 0);
    assert(stack.seg_first == 272 && stack.seg_max == 272 && stack.cache_max == 1);
    return 0;
}
