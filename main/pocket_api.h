#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "quickjs.h"

// globalThis.pocket — the common JS API surface described in docs/common-api.md.
//
// This is stage A of that document and covers only the shared foundation:
// apiVersion, device.info, capabilities and cancel, plus the PocketError shape
// every later API is expected to reject and throw with. Nothing here replaces
// the existing ui.createNode globals; those keep working as legacy-pocketjs and
// are untouched.
//
// Everything in this header runs on the JS owner task. The registry itself is
// plain data and may be filled in from any task before a session starts, but
// pocket_api_capability_changed() calls into JS and must not be reached from an
// ISR, a driver task or a worker.

#define POCKET_API_VERSION "0.1.0"

// The common error codes of docs/common-api.md section 4. The JS side sees the
// same strings through pocket.errorCodes.
#define POCKET_ERR_INVALID_ARGUMENT  "INVALID_ARGUMENT"
#define POCKET_ERR_UNSUPPORTED       "UNSUPPORTED"
#define POCKET_ERR_NOT_AVAILABLE     "NOT_AVAILABLE"
#define POCKET_ERR_PERMISSION_DENIED "PERMISSION_DENIED"
#define POCKET_ERR_BUSY              "BUSY"
#define POCKET_ERR_LIMIT_EXCEEDED    "LIMIT_EXCEEDED"
#define POCKET_ERR_OUT_OF_MEMORY     "OUT_OF_MEMORY"
#define POCKET_ERR_TIMEOUT           "TIMEOUT"
#define POCKET_ERR_CANCELLED         "CANCELLED"
#define POCKET_ERR_CLOSED            "CLOSED"
#define POCKET_ERR_DISCONNECTED      "DISCONNECTED"
#define POCKET_ERR_NOT_FOUND         "NOT_FOUND"
#define POCKET_ERR_CORRUPT_DATA      "CORRUPT_DATA"
#define POCKET_ERR_IO_ERROR          "IO_ERROR"
#define POCKET_ERR_AUTH_FAILED       "AUTH_FAILED"
#define POCKET_ERR_TLS_ERROR         "TLS_ERROR"
#define POCKET_ERR_CONFLICT          "CONFLICT"

// outcome values. Omit for operations that changed nothing observable.
#define POCKET_OUTCOME_NOT_APPLIED "not-applied"
#define POCKET_OUTCOME_APPLIED     "applied"
#define POCKET_OUTCOME_UNKNOWN     "unknown"

// Reason strings for capability.reason. Any short upper-case token works; these
// are the ones the document names.
#define POCKET_REASON_NO_DEVICE       "NO_DEVICE"
#define POCKET_REASON_DISABLED        "DISABLED"
#define POCKET_REASON_BUSY            "BUSY"
#define POCKET_REASON_NOT_IMPLEMENTED "NOT_IMPLEMENTED"

typedef enum {
    POCKET_LIMIT_END = 0,   // terminates a limits array
    POCKET_LIMIT_INT,
    POCKET_LIMIT_TEXT,
    POCKET_LIMIT_FLAG,
} pocket_limit_kind_t;

// One entry of Capability.limits. Arrays are static const and end with a zeroed
// element, so a limits table costs flash only.
typedef struct {
    const char         *name;
    pocket_limit_kind_t kind;
    int32_t             number;     // INT value, or FLAG as 0/1
    const char         *text;       // TEXT value
} pocket_limit_t;

typedef struct pocket_capability pocket_capability_t;

// Optional live check. Called on the JS task for every capabilities.get().
// `available` starts at the static value and `reason` at the static reason;
// write either to report the current state. Keep it cheap and side-effect free:
// the document treats available as an observation, not a reservation.
typedef void (*pocket_capability_probe_fn)(const pocket_capability_t *cap,
                                           bool *available,
                                           const char **reason);

struct pocket_capability {
    const char                *name;        // "sensors.imu" etc. Not copied.
    bool                       supported;   // implemented in this firmware
    bool                       available;   // static fallback when probe is NULL
    const char                *reason;      // static fallback, NULL for none
    const pocket_limit_t      *limits;      // optional, NULL for {}
    pocket_capability_probe_fn probe;       // optional
    void                      *user_data;
};

// Publishes a capability, replacing any entry with the same name. The struct
// must outlive the registration; point it at a static const. Safe to call
// before a session starts. Returns ESP_ERR_NO_MEM when the table is full.
esp_err_t pocket_api_register(const pocket_capability_t *capability);

// Tells subscribers that a capability's observable state moved. No-op when no
// realm is live. JS owner task only.
void pocket_api_capability_changed(const char *name);

// Registers globalThis.pocket. Pass to pocketjs_guest_quickjs_install_once().
esp_err_t pocket_api_install(JSContext *ctx, void *user_data);

// The pocket root, for surfaces that add their own namespace to it. Returns
// JS_UNDEFINED when the API is not installed. The caller frees the value.
JSValue pocket_api_root(JSContext *ctx);

// Builds a PocketError. `message` may be NULL, in which case the code is used.
// `outcome` may be NULL to leave the property off. pocket_api_throw() is the
// same thing already thrown, for synchronous functions; the plain form is what
// a Promise rejection wants.
JSValue pocket_api_error(JSContext *ctx, const char *code, const char *operation,
                         const char *message, bool retryable, const char *outcome);
JSValue pocket_api_throw(JSContext *ctx, const char *code, const char *operation,
                         const char *message, bool retryable, const char *outcome);

// Cancel tokens are host-made opaque objects; JS cannot forge one. Use these to
// validate an Options.cancel and to poll it.
bool pocket_api_is_cancel_token(JSValueConst value);
bool pocket_api_cancel_requested(JSValueConst token);
