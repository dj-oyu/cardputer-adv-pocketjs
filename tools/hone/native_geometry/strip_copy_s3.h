#ifndef HONE_STRIP_COPY_S3_H
#define HONE_STRIP_COPY_S3_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(HONE_PIE_STRIP_COPY) && HONE_PIE_STRIP_COPY
/* Task-context only. Keep the zero-overhead loop outside any caller's
 * hardware loop. Q0/Q1 are private to this call, like the HAL swap kernel. */
static __attribute__((noinline)) void hone_strip_copy_blocks(void *dst, const void *src,
                                                            size_t blocks) {
    __asm__ volatile (
        "loopgtz %[blocks], 1f\n\t"
        "ee.vld.128.ip q0, %[src], 16\n\t"
        "ee.vld.128.ip q1, %[src], 16\n\t"
        "ee.vst.128.ip q0, %[dst], 16\n\t"
        "ee.vst.128.ip q1, %[dst], 16\n\t"
        "1:"
        : [src] "+&a"(src), [dst] "+&a"(dst)
        : [blocks] "a"(blocks)
        : "memory");
}
#endif

/* Source and destination belong to distinct, CPU-accessible strip buffers. */
static inline void hone_strip_copy(void *dst, const void *src, size_t bytes) {
#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(HONE_PIE_STRIP_COPY) && HONE_PIE_STRIP_COPY
    if (bytes >= 32 && !(bytes & 31) && !(((uintptr_t)src | (uintptr_t)dst) & 15)) {
        hone_strip_copy_blocks(dst, src, bytes / 32);
        return;
    }
#endif
    memcpy(dst, src, bytes);
}
#endif
