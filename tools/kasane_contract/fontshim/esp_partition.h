#pragma once
#include "esp_err.h"
#include <stddef.h>
typedef struct { size_t size; } esp_partition_t;
typedef unsigned esp_partition_mmap_handle_t;
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_MMAP_DATA 0
static inline const esp_partition_t *esp_partition_find_first(int type,int subtype,const char *name){
    (void)type;(void)subtype;(void)name;return NULL;
}
static inline esp_err_t esp_partition_mmap(const esp_partition_t *p,size_t offset,size_t size,int mode,
    const void **base,esp_partition_mmap_handle_t *handle){
    (void)p;(void)offset;(void)size;(void)mode;(void)base;(void)handle;return ESP_FAIL;
}
