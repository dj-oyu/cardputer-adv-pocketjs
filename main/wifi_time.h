#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

// One-shot Wi-Fi clock synchronization. Connects, asks SNTP for the time,
// hands it to solar_time, and takes the whole radio stack back down again.
//
// Nothing here stays connected. docs/architecture.md:122 budgets the radio as
// something to switch on and measure, and the guest heap is the scarce resource
// on a board with no PSRAM, so the design is a task that runs to completion and
// exits rather than a service that holds an association. The lasting cost is the
// Wi-Fi driver's .bss, which the linker charges once the component is in the
// image whether the radio ever starts or not.
//
// This module never handles timezones. SNTP writes UTC through settimeofday()
// and solar_time.c reads that directly (docs/solar-sail.md).

#define WIFI_TIME_SSID_MAX 32   // 802.11 SSID: 32 bytes, no NUL on the wire
#define WIFI_TIME_PSK_MAX  64   // WPA2 passphrase 8..63, or a 64 hex digit PSK

typedef enum {
    WIFI_TIME_IDLE,      // no sync attempted since boot
    WIFI_TIME_RUNNING,
    WIFI_TIME_OK,        // the system clock was set from the network
    WIFI_TIME_FAILED
} wifi_time_state_t;

// How far the attempt got. The UI shows this rather than a bare error, because
// "wrong password" and "no such network" want different things from the user.
typedef enum {
    WIFI_TIME_STAGE_NONE,
    WIFI_TIME_STAGE_NVS,     // no stored credentials, or NVS itself is unusable
    WIFI_TIME_STAGE_INIT,    // netif / event loop / driver bring-up
    WIFI_TIME_STAGE_ASSOC,   // scanning and associating: AP not found, too weak
    WIFI_TIME_STAGE_AUTH,    // the AP answered and rejected us: wrong PSK
    WIFI_TIME_STAGE_DHCP,    // associated, no address
    WIFI_TIME_STAGE_SNTP     // addressed, no usable answer from the servers
} wifi_time_stage_t;

typedef struct {
    wifi_time_state_t state;
    wifi_time_stage_t stage;
    // At ASSOC and AUTH this is a wifi_err_reason_t from the disconnect event;
    // elsewhere an esp_err_t. Zero when the stage carries no further detail.
    int  reason;
    int  rssi;                // 0 until associated
    char ip[16];              // "" until DHCP completes
} wifi_time_status_t;

// Stable lower-case token for the log marker and for the UI, e.g. "auth".
const char *wifi_time_stage_name(wifi_time_stage_t stage);

// Credentials live in NVS namespace "wifi", keys "ssid" and "psk".
//
// The PSK goes in and never comes back out: there is no getter for it, and no
// code path logs it at any level. docs/common-api.md:73 requires that a Wi-Fi
// password stay out of the ordinary logs, and a truncated password is still a
// password with its search space cut down.
esp_err_t wifi_time_credentials_set(const char *ssid, const char *psk);
esp_err_t wifi_time_credentials_clear(void);
bool      wifi_time_has_credentials(void);
// Copies the stored SSID, always NUL terminating. ESP_ERR_NVS_NOT_FOUND when
// nothing is stored; out is set to "" in that case so the UI can print it.
esp_err_t wifi_time_ssid_get(char *out, size_t size);

// Starts the one-shot task. Returns immediately; poll wifi_time_status().
// ESP_ERR_INVALID_STATE if an attempt is already running.
esp_err_t wifi_time_sync_start(void);

// Safe from any task at any time.
wifi_time_status_t wifi_time_status(void);
