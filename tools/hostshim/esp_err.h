#pragma once
// The slice of ESP-IDF the editor touches, for host builds only.
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERROR_CHECK(x) do { esp_err_t e_=(x); if(e_!=ESP_OK) __builtin_trap(); } while(0)
