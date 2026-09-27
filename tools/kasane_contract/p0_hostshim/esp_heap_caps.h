#pragma once
#include <stddef.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
static inline size_t heap_caps_get_free_size(unsigned caps){(void)caps;return 0;}
static inline size_t heap_caps_get_minimum_free_size(unsigned caps){(void)caps;return 0;}
static inline size_t heap_caps_get_largest_free_block(unsigned caps){(void)caps;return 0;}
