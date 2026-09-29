#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

// A reference-counted Wi-Fi link for native code that wants the network for a
// while and does not care who else does. The first acquire brings the link up
// through wifi_time.c; the last release takes it down again. Nothing here syncs
// a clock, parses a URL or knows about JavaScript.
//
// Why this exists rather than each user calling wifi_time_link_start(): the
// link is one radio behind one lock, and every caller of it so far (the clock,
// pocket.net) has had to reason about who else might hold it and who is allowed
// to stop it. A count answers "stop it now?" once, here. The clock is the first
// holder (net_autosync.c); a resident background service is the one this is
// shaped for (docs/platform/wifi-autostart.md), and is not built yet.
//
// What it does NOT do, on purpose, in this build:
//   - share a link with pocket.net. An app's lease is still its own link
//     (WIFI_TIME_LINK_APP), and the two are mutually exclusive through
//     wifi_time.c's lock: whoever asks second is refused, and neither can stop
//     the other's (wifi_time_link_stop_for). Today the only holder yields to
//     every app start before the guest exists, so the two never meet; the
//     document lists what sharing would need.
//   - retry. A link that fails stays FAILED until a holder acquires again.
//
// Holders name themselves. The name is how a log line says who kept the radio
// up, which is the first question when the home screen is 50 KB short.

typedef enum {
    NET_SERVICE_OFF,          // no holder, or the link is on its way down
    NET_SERVICE_CONNECTING,
    NET_SERVICE_UP,           // associated with an address
    NET_SERVICE_FAILED,       // held, but the link never came up or was lost
} net_service_state_t;

// How many distinct holder names can be counted at once. Each name has its own
// count, so the same name may acquire more than once.
#define NET_SERVICE_REASONS 4
#define NET_SERVICE_LISTENERS 4

// Once, at boot, before any task can acquire: creates the lock.
void net_service_init(void);

// Counts one hold for `reason`, and starts the link if nothing is keeping one
// up. Safe from any task. `reason` must outlive the hold (a string literal).
//
//   ESP_OK                 counted; poll net_service_state() for UP
//   ESP_ERR_INVALID_ARG    NULL or empty reason
//   ESP_ERR_NO_MEM         NET_SERVICE_REASONS names are already holding
//   ESP_ERR_INVALID_STATE  the radio belongs to someone else (the settings
//                          screen's sync or scan, or an app's lease); nothing
//                          was counted
//   other                  the link task could not be created; nothing counted
esp_err_t net_service_acquire(const char *reason);

// Gives one hold back; the last one stops the link. A release with no matching
// acquire is refused (ESP_ERR_INVALID_STATE) and logged rather than allowed to
// drive the count below zero, which would take the radio from a holder that
// still wants it.
esp_err_t net_service_release(const char *reason);

net_service_state_t net_service_state(void);
const char *net_service_state_name(net_service_state_t state);
unsigned net_service_holders(void);   // total holds across every name

// "time=1 bg=2", for a log line. Always NUL terminated; "" with no holders.
void net_service_describe(char *out, size_t size);

// Change notification. Listeners are called from net_service_pump(), on
// whichever task pumps it (the UI task), once per change of state -- never from
// the link task, which must not run someone else's code while it holds the
// radio lock. Returns false when the table is full.
typedef void (*net_service_listener_t)(net_service_state_t state, void *user);
bool net_service_listen(net_service_listener_t fn, void *user);
void net_service_unlisten(net_service_listener_t fn, void *user);
void net_service_pump(void);
