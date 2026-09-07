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

// Whether the driver is initialised right now. A capability probe needs this to
// tell "the radio is already up, so using it is free" from "the radio has to be
// brought up, which costs about 48 KB and fails outright below that". Safe from
// any task; it is a plain read of a flag the attempt task owns.
bool wifi_time_radio_is_up(void);

// ------------------------------------------------------------------ scanning
//
// So the SSID is chosen from what is actually in the air rather than typed. A
// typed SSID fails as WIFI_REASON_NO_AP_FOUND whether the network is absent or
// the spelling is wrong, and those want opposite things from the person.

#define WIFI_TIME_SCAN_MAX 16   // docs/common-api.md:306 caps a scan at 16

typedef struct {
    char   ssid[WIFI_TIME_SSID_MAX+1];
    int8_t rssi;
    bool   secure;               // anything but an open network
} wifi_time_network_t;

// Brings the radio up, scans, and takes it down again, like the sync task and
// under the same single-attempt lock: a scan and a sync cannot overlap.
// Returns immediately; poll wifi_time_scan_state().
esp_err_t wifi_time_scan_start(void);
wifi_time_state_t wifi_time_scan_state(void);

// Copies up to max networks, strongest first, into out; returns how many. One
// entry per SSID — a mesh answering from three radios is one network to choose.
// truncated, when given, reports that something was dropped: more than
// WIFI_TIME_SCAN_MAX distinct networks, or an SSID that was not valid UTF-8 and
// so could not be drawn or stored (docs/common-api.md:306).
unsigned wifi_time_scan_networks(wifi_time_network_t *out, unsigned max,
                                 bool *truncated);

// ---------------------------------------------------------------- the link
//
// A held association, for pocket_net.c. Same radio_up()/tear_down() pair as the
// clock and the scan and, through the same single-attempt lock, mutually
// exclusive with both: whoever asks second gets ESP_ERR_INVALID_STATE. Nothing
// here syncs a clock or touches solar_time.
//
// This is the only part of this module that stays up. It exists because HTTP
// wants the radio for the length of a request and possibly across several, and
// paying a multi-second association per request would be worse for both the
// battery and the app. The lease is the app's to end; wifi_time_link_stop()
// and a session ending are the only things that take it away.

typedef enum {
    WIFI_TIME_LINK_DOWN,        // no link held
    WIFI_TIME_LINK_CONNECTING,
    WIFI_TIME_LINK_UP,          // associated with an address
    WIFI_TIME_LINK_FAILED       // never came up, or the AP took it away
} wifi_time_link_t;

// Starts the link task. Returns immediately; poll wifi_time_link_state().
// ESP_ERR_INVALID_STATE when a sync, a scan or another link already has the
// radio. Uses the credentials in NVS, exactly as the clock does.
esp_err_t wifi_time_link_start(void);

// Asks for the link back. Non-blocking and idempotent: the task tears the radio
// down and clears the lock on its own. The stage of a failure is in
// wifi_time_status(), which the link fills the same way a sync does.
void wifi_time_link_stop(void);

wifi_time_link_t wifi_time_link_state(void);

// Copies the address the link holds, always NUL terminating; "" when it has
// none. Safe from any task.
void wifi_time_link_ip(char *out, size_t size);
