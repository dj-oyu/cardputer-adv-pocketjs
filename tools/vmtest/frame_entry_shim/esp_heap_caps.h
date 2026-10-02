#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_8BIT 4
void *frame_entry_heap_malloc(size_t n);
void frame_entry_heap_free(void *p);
static inline void *heap_caps_malloc(size_t n, unsigned caps) {(void)caps;return frame_entry_heap_malloc(n);}
static inline void heap_caps_free(void *p) {frame_entry_heap_free(p);}
