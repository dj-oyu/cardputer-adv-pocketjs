#pragma once
// The slice of ESP-IDF the editor touches, for host builds only.
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERROR_CHECK(x) do { esp_err_t e_=(x); if(e_!=ESP_OK) __builtin_trap(); } while(0)
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
// Enough of it for a log line. The names are the ones the codes above carry in
// ESP-IDF; anything else prints as a number, which is what a host test needs to
// read a message rather than to act on it.
static inline const char *esp_err_to_name(esp_err_t err) {
    switch(err) {
        case ESP_OK: return "ESP_OK";
        case ESP_FAIL: return "ESP_FAIL";
        case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
        case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
        default: return "ESP_ERR";
    }
}
