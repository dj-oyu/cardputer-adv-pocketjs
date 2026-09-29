#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Automatic clock sync at boot, and again every twelve hours for a device left
// running. It is a short, transient use of the radio, never a standing one:
// docs/platform/wifi-autostart.md has the reasoning and the measurements this
// still needs from the device.
//
//   when   the home screen has been idle (no key, no app, no modal, no
//          overlay starting) for AUTOSYNC_IDLE_MS, credentials are stored, the
//          AUTO TIME SYNC setting is on, and at least AUTOSYNC_MIN_FREE is free
//   how    its own task: net_service_acquire("time") -> link up -> SNTP ->
//          release -> wait for the radio lock to clear
//   ends   on success (the clock is marked synchronized, the only side effect),
//          on failure (backoff 1 / 5 / 30 min, AUTOSYNC_MAX_FAILURES in a row
//          ends it for the boot), or on net_autosync_yield()
//
// APPS FIRST. Every path that builds a guest or leaves the home screen calls
// net_autosync_yield() before it does, and that call returns only once the
// radio is down and its heap is back. A sync is never started while an app or
// another screen owns the display, so a game never meets the radio's 48 KB.

// ------------------------------------------------------------------ policy
//
// Pure: no FreeRTOS, no clock of its own, no heap. The host test drives it with
// a fake clock (tools/test_net_autosync.c).

#define AUTOSYNC_IDLE_MS          10000     // home quiet this long before the radio
#define AUTOSYNC_MIN_FREE         (64*1024) // see net_autosync.c
#define AUTOSYNC_BACKOFF_1_MS     (60*1000)
#define AUTOSYNC_BACKOFF_2_MS     (5*60*1000)
#define AUTOSYNC_BACKOFF_3_MS     (30*60*1000)
#define AUTOSYNC_MAX_FAILURES     4         // in a row: the first try and 3 retries
#define AUTOSYNC_RESYNC_MS        (12LL*60*60*1000)
#define AUTOSYNC_RETRY_MS         (60*1000) // after a yield, a busy radio or low memory
#define AUTOSYNC_MAX_ATTEMPTS     32        // per boot, whatever their outcome

typedef enum {
    AUTOSYNC_OK,          // the clock was set
    AUTOSYNC_FAILED,      // the network or SNTP did not answer
    AUTOSYNC_ABORTED,     // yielded to an app or a screen
    AUTOSYNC_BUSY,        // the radio belonged to someone else
} autosync_outcome_t;

typedef enum {
    AUTOSYNC_WAIT,        // nothing to do this frame
    AUTOSYNC_START,       // start an attempt now (counted)
    AUTOSYNC_LOW_MEMORY,  // due, but not enough heap; deferred, not counted
    AUTOSYNC_NO_CREDENTIALS, // due, but nothing stored; deferred, not counted
} autosync_decision_t;

typedef struct {
    unsigned attempts;    // started this boot
    unsigned failures;    // in a row; a success resets it
    int64_t  next_ms;     // earliest time the next attempt may start
    int64_t  last_ok_ms;  // -1 until the first success
    bool     running;
    bool     gave_up;     // for this boot, or until the setting is turned on again
} autosync_policy_t;

typedef struct {
    bool    enabled;          // the AUTO TIME SYNC setting
    bool    has_credentials;
    int64_t idle_ms;          // how long the home screen has been eligible; <0 not now
    size_t  free_bytes;       // internal 8-bit heap free
} autosync_inputs_t;

void autosync_policy_init(autosync_policy_t *p);
autosync_decision_t autosync_policy_decide(autosync_policy_t *p,
                                           const autosync_inputs_t *in,
                                           int64_t now_ms);
void autosync_policy_record(autosync_policy_t *p, autosync_outcome_t outcome,
                            int64_t now_ms);
const char *autosync_outcome_name(autosync_outcome_t outcome);

// ----------------------------------------------------------------- attempt
//
// One attempt, run to its end on the calling task. The hooks are what differs
// between the device and the host test; the link goes through net_service.c
// on both, against wifi_time.c on the device and a fake of it on the host.

typedef struct {
    int64_t   (*now_ms)(void);
    void      (*sleep_ms)(unsigned ms);
    bool      (*abort_requested)(void);
    esp_err_t (*sntp_start)(void);
    // One slice of the wait. ESP_OK once the clock has been stepped,
    // ESP_ERR_TIMEOUT when the slice ran out first, anything else is final.
    esp_err_t (*sntp_wait)(unsigned slice_ms);
    void      (*sntp_stop)(void);
    void      (*set_synchronized)(void);
    bool      (*radio_busy)(void);   // the radio lock, for "is the heap back"
} autosync_ops_t;

#define AUTOSYNC_SLICE_MS         100       // how often an attempt looks for a yield
#define AUTOSYNC_CONNECT_WAIT_MS  20000     // outer bound; the link's own is 15 s
#define AUTOSYNC_SNTP_WAIT_MS     10000     // the manual sync's SNTP_TIMEOUT_MS
#define AUTOSYNC_DOWN_WAIT_MS     3000      // release to radio lock clear

autosync_outcome_t autosync_attempt(const autosync_ops_t *ops);

// ----------------------------------------------------------------- runtime

// Once, at boot, after NVS.
void net_autosync_init(void);

// Every frame from the UI task. `eligible` is the home screen with nothing in
// front of it and no app running; `key` is a keystroke this frame. Cheap when
// nothing is due: two comparisons and a clock read.
void net_autosync_poll(bool eligible, bool key);

// Gives the radio back before something that needs the heap. Blocks the caller
// until the attempt has torn the radio down (a slice plus the teardown), or for
// at most AUTOSYNC_YIELD_WAIT_MS. A no-op returning at once when no attempt is
// running. `why` goes in the log.
#define AUTOSYNC_YIELD_WAIT_MS 3000
void net_autosync_yield(const char *why);

// The AUTO TIME SYNC setting. Default on. Turning it on resets the policy, so
// it is also how a person asks for another try after the boot gave up.
bool net_autosync_enabled(void);
void net_autosync_set_enabled(bool on);
