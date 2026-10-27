#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Native, owner-task control. See docs/api/app-control-v1.md. No JS objects. */
enum { AC_CATALOG=1, AC_OPEN, AC_START, AC_TAP, AC_STATUS, AC_SUSPEND, AC_STOP, AC_CLOSE, AC_REPLY=128 };
enum { AC_OK=0, AC_ACCEPTED, AC_CONSUMED, AC_UNKNOWN_APP, AC_UNSUPPORTED,
       AC_STALE_BOOT, AC_STALE_SESSION, AC_STALE_LEASE, AC_STALE_RUN,
       AC_OUT_OF_ORDER, AC_CONFLICT, AC_QUEUE_FULL, AC_BLOCKED, AC_BUSY,
       AC_OUTCOME_UNKNOWN, AC_LIMIT, AC_EXPIRED, AC_INVALID, AC_TEARDOWN_FAILED };
enum { AC_NO_SESSION=0, AC_READY, AC_RUNNING, AC_SUSPENDED, AC_STOPPED, AC_NEEDS_REVIEW };
enum { AC_REQUEST_SIZE=60, AC_RESPONSE_SIZE=120, AC_INBOX=4, AC_LEDGER=64 };
typedef struct {
    const char *id, *title;
    uint32_t flags;
} app_control_descriptor_t;
typedef struct {
    unsigned (*catalog_count)(void);
    bool (*catalog)(unsigned index, app_control_descriptor_t *out);
    uint8_t (*start)(const char *id, bool resume);
    uint8_t (*tap)(uint8_t action);
    uint8_t (*suspend)(void);
    bool (*stop)(void);
    bool (*emit)(const uint8_t *body, size_t length);
    void (*fingerprint)(uint8_t hash[32], char version[48]);
    uint32_t (*instance)(void);
    uint64_t (*clock_ms)(void);  // optional; recheck TTL between blocking owner calls
} app_control_hooks_t;
/* Initialize before input/UI tasks. boot must be fresh and nonzero per boot. */
void app_control_init(uint64_t boot, const app_control_hooks_t *hooks);
/* Single producer (USB input). Body already passed bounded codec/CRC checks. */
void app_control_receive(const uint8_t *body, size_t length);
/* Single UI owner; monotonic milliseconds, called even at HOME. */
void app_control_pump(uint64_t now_ms);
/* Physical/guest lifecycle changes invalidate queued control input. Owner only. */
void app_control_observe(bool running, const char *dormant_id, const char *current_id, uint32_t instance);
size_t app_control_static_bytes(void);
