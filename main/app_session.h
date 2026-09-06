#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
esp_err_t app_start(void);
esp_err_t app_start_test(char test);
void app_force_redraw(void);
esp_err_t app_tick(uint32_t buttons);
void app_stop(void);
void app_request_stop(void);
void app_report(void);
