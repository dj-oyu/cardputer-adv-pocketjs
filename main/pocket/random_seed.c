#include "random_stream.h"
#ifndef POCKET_DERBY_COMPARE_FIXED_HW
#include "esp_random.h"
#else
/* A retained ELF value returned by the real provider, for independent proof. */
const volatile uint32_t pocket_derby_comparison_hw_seed = UINT32_C(0x2545F491);
#endif

uint32_t pocket_random_seed(void) {
#ifdef POCKET_DERBY_COMPARE_FIXED_HW
    /* Diagnostic comparison images only: host DERBY_HW's existing default. */
    return pocket_derby_comparison_hw_seed;
#else
    return esp_random();
#endif
}
