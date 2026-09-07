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

// Section 4 puts argument errors from a Promise-returning method into the
// rejection rather than a throw, so a surface whose work is synchronous still
// has to hand back a Promise. pocket_api_settled() builds one that is already
// settled with `value` (which it takes over), and pocket_api_reject() is the
// same thing carrying a PocketError.
JSValue pocket_api_settled(JSContext *ctx, JSValue value, bool rejected);
JSValue pocket_api_reject(JSContext *ctx, const char *code, const char *operation,
                          const char *message, bool retryable, const char *outcome);

// ------------------------------------------------------------- subscriptions
//
// capabilities.onChange, sensors.imu.watch and power.onChange were the same
// bookkeeping three times over: a fixed table of slots, a handle that makes
// close() idempotent, and a delivery loop that survives a listener closing
// subscriptions from inside its own call. That bookkeeping lives here now, and
// a surface is left with its payload and its own pacing.
//
// The slot array belongs to the surface, so per-subscription state the surface
// needs -- a watch's period, a poll's freshness -- stays in a parallel array of
// its own indexed by the same slot number.
//
// Everything here runs on the JS owner task.

typedef struct {
    JSValue  callback;   // JS_UNDEFINED when the slot is free
    uint32_t handle;     // 0 marks a free slot
} pocket_sub_slot_t;

typedef struct pocket_sub_table pocket_sub_table_t;

// Called after a slot opened or closed, so a surface can follow demand for the
// hardware its subscriptions drive. Optional.
typedef void (*pocket_sub_changed_fn)(pocket_sub_table_t *table);

struct pocket_sub_table {
    pocket_sub_slot_t    *slots;    // the surface's array; not owned
    int                   count;
    JSContext            *ctx;      // the realm the callbacks belong to
    const char           *tag;      // ESP log tag for a listener that throws
    const char           *what;     // "watch", "onChange", ... for that same line
    // Whether a listener that throws loses its subscription. The two polled
    // surfaces say yes: a listener throwing every frame would otherwise fill
    // the log and keep costing a call for as long as the program runs.
    // capabilities.onChange says no -- it fires only when a capability moves,
    // so there is no runaway to stop -- and that difference is deliberate.
    bool                  close_on_throw;
    pocket_sub_changed_fn changed;
    void                 *user;     // for `changed`
    unsigned              open;     // slots in use
    uint32_t              next_handle;
};

// Opens a slot and builds the Subscription of section 4, whose close() is bound
// to both the slot and the handle that slot held. Throws rather than rejects:
// every onChange and watch on this host is a synchronous function. `operation`
// and `full` name the LIMIT_EXCEEDED refusal when the table has no free slot.
// `slot_out` may be NULL; when it is not, it receives the slot the surface
// should fill its own parallel entry for.
JSValue pocket_api_sub_open(JSContext *ctx, pocket_sub_table_t *table,
                            JSValueConst listener, const char *operation,
                            const char *full, int *slot_out);

// Closes one slot, or every slot. Both are idempotent and both run `changed`.
void pocket_api_sub_close(pocket_sub_table_t *table, int slot);
void pocket_api_sub_close_all(pocket_sub_table_t *table);

// Builds the payload one listener is about to receive. Return false to skip
// this slot for this round -- that is where a surface's pacing lives.
typedef bool (*pocket_sub_payload_fn)(JSContext *ctx, int slot, void *user,
                                      JSValue *payload);

// Delivers to every open listener. A listener may close subscriptions, this one
// included, during its own call, so each slot is re-read and its callback held
// across the call.
void pocket_api_sub_deliver(pocket_sub_table_t *table,
                            pocket_sub_payload_fn build, void *user);

// For a surface whose table hangs off a GC-marked hub object: a listener that
// closes over its own namespace makes a cycle back to it, and marking lets the
// collector break that.
void pocket_api_sub_mark(pocket_sub_table_t *table, JSRuntime *rt,
                         JS_MarkFunc *mark);

// --------------------------------------------------------- async completions
//
// A driver task -- or an ISR -- finishes work and posts a status; the JS task
// turns that into a Promise resolution in pocket_api_pump(), because section 5
// forbids anything but the JS owner task from touching QuickJS.
//
// The request number is the appSessionId, generation and requestId of section 5
// rolled into one number, and this is the place that says so. Its low bits are
// the index of the slot it owns, so posting a completion is a lookup and two
// atomic stores with no queue to overflow and nothing to drop -- section 5 asks
// the completion path not to lose a settlement quietly. The high bits come from
// a counter that is NEVER reset: not by pocket_api_reset(), not by a new
// session. A request number is therefore unique for the life of the firmware
// run, so a driver still working for a session that has ended posts a number
// that matches no slot and its completion is simply never read. That is how an
// ended session's results are discarded, and it is why nothing has to chase a
// worker down. Do not "tidy" the counter into being reset per session.

typedef uint32_t pocket_request_t;   // 0 is never a valid request

// The status a driver posts. 0 means the work ran to completion; every other
// value is the driver's own, and only the surface that started it knows what it
// means.
#define POCKET_STATUS_OK 0

typedef struct {
    // Builds what the Promise settles with, and takes over the value's life.
    // `status` is what the driver posted. `stop_code` is NULL unless the host
    // asked the work to stop early, in which case it is the PocketError code
    // that stop was made for -- and the status still says whether the work had
    // already finished by then, which is what an honest outcome needs.
    JSValue (*settle)(JSContext *ctx, void *user, int32_t status,
                      const char *stop_code, bool *rejected);
    // Asks the driver to stop, for a cancelled token, an expired deadline or a
    // session that is ending. The Promise is not settled here: section 4 gives
    // the host the wait for the native stop, and the driver still owns the
    // request until its completion lands. Optional.
    void (*stop)(void *user, const char *code);
    // Runs after the slot is free, settled or not. Optional.
    void (*release)(void *user);
} pocket_promise_ops_t;

// Claims a slot and its request number without touching JS, so a driver can be
// started -- and can finish -- before the Promise exists. Returns 0 when every
// slot is in flight. Follow it with exactly one of _arm or _abandon.
pocket_request_t pocket_api_promise_open(void);

// Gives a claimed slot back without settling anything. For a call that failed
// between _open and _arm; the driver's completion then matches no request.
void pocket_api_promise_abandon(pocket_request_t request);

// Builds the Promise the app receives and arms the slot. Takes over `cancel`
// (JS_UNDEFINED when the call passed no token) either way. On failure the slot
// is given back and the exception is returned; stopping the driver is the
// caller's to do, because only it holds the handle the driver was started with.
JSValue pocket_api_promise_arm(JSContext *ctx, pocket_request_t request,
                               const pocket_promise_ops_t *ops, void *user,
                               JSValue cancel, int64_t deadline_us);

// Posts a driver's outcome. Safe from any task and from an ISR: it takes no
// lock, allocates nothing, never blocks and touches no JS value -- one load and
// two atomic stores into a slot this request already owns. An ISR must still do
// nothing else with the API.
//
// The one cost worth knowing: this lives in flash, not IRAM, so an interrupt
// that can fire while the cache is off -- an ESP_INTR_FLAG_IRAM handler during
// a flash write -- must not call it. Such a handler should hand the request to
// a task that does. Nothing needs this today; the day something does, the fix
// is an IRAM_ATTR on this function alone, not a queue.
void pocket_api_complete(pocket_request_t request, int32_t status);

// Settles what the drivers finished, and enforces cancel and timeoutMs for what
// they have not. Call once per frame from the JS task. Costs one load and one
// branch when nothing is in flight.
void pocket_api_pump(void);

// Ends the session's promises: each is asked to stop and its resolvers are
// released without settling, there being nobody left to settle to. Call from
// the JS task while the guest is still alive -- app_stop() before it destroys
// the guest -- next to the surfaces' own resets.
void pocket_api_reset(void);
