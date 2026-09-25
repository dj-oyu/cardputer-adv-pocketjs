#pragma once

#include <stddef.h>

#define MALLOC_CAP_INTERNAL 1u
#define MALLOC_CAP_8BIT 2u

void *heap_caps_calloc(size_t count, size_t size, unsigned caps);
void heap_caps_free(void *ptr);
