#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef void *esp_timer_handle_t;
typedef struct {void (*callback)(void *); const char *name;} esp_timer_create_args_t;
int64_t esp_timer_get_time(void);
static inline esp_err_t esp_timer_create(const esp_timer_create_args_t *a,esp_timer_handle_t *h){(void)a;*h=0;return ESP_OK;}
static inline esp_err_t esp_timer_stop(esp_timer_handle_t h){(void)h;return ESP_OK;}
static inline esp_err_t esp_timer_delete(esp_timer_handle_t h){(void)h;return ESP_OK;}
static inline esp_err_t esp_timer_start_once(esp_timer_handle_t h,uint64_t t){(void)h;(void)t;return ESP_OK;}
