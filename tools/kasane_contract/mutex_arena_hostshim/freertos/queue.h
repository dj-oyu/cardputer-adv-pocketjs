#pragma once

#include <stdint.h>

typedef struct { uint8_t storage[84]; } StaticQueue_t;
typedef void *QueueHandle_t;

QueueHandle_t xQueueCreateMutexStatic(uint8_t type, StaticQueue_t *storage);
