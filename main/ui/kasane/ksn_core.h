#ifndef KSN_CORE_H
#define KSN_CORE_H
#include "ksn_api.h"
#include "ksn_ports.h"
#include "ksn_composition_types.h"
#define KSN_RESOURCES 16u

#define KSN_APP_COMMANDS 80u
#define KSN_SYSTEM_COMMANDS 16u
#define KSN_COMMANDS (KSN_APP_COMMANDS + KSN_SYSTEM_COMMANDS)
#define KSN_APP_TEXT_BYTES 896u
#define KSN_SYSTEM_TEXT_BYTES 128u
#define KSN_TEXT_BYTES (KSN_APP_TEXT_BYTES + KSN_SYSTEM_TEXT_BYTES)
#define KSN_CORE_STORAGE_BYTES 9216u

/* Host storage layout, not a guest API. Do not access fields directly.
 * A typed member avoids accessing a declared byte array as an unrelated struct. */
typedef struct ksn_core_impl ksn_core_impl;
typedef struct { ksn_core_impl *core; ksn_layer layer; } ksn_endpoint;
typedef struct { ksn_image_port port; ksn_resource id; ksn_layer layer; } ksn_image_entry;
typedef struct {
    ksn_command_storage *commands;
    uint8_t *text;
    uint16_t count[2],text_used[2];
    uint32_t generation[2];
    ksn_rgba background[2];
    bool background_set[2];
} ksn_bank;
struct ksn_core_impl {
    ksn_bank banks[2];
    ksn_endpoint endpoints[2];
    ksn_image_entry images[KSN_RESOURCES];
    uint8_t image_count;
    ksn_tx transaction;
    ksn_submission outcome;
    ksn_result poison;
    ksn_layer layer;
    ksn_update_mode mode;
    uint8_t active,building_bank;
    bool building,submitted,full_redraw,repairing,invalidated;
};

/* Borrowed, immovable blocks. The core itself never allocates. */
typedef struct { ksn_command_storage commands[KSN_COMMANDS]; } ksn_core_command_block;
typedef struct { uint8_t bytes[KSN_TEXT_BYTES]; } ksn_core_text_block;
typedef struct {
    ksn_core_impl state;
} ksn_core;
#define KSN_CORE_RESERVED_BYTES (sizeof(ksn_core)+2*sizeof(ksn_core_command_block)+2*sizeof(ksn_core_text_block))

#ifdef __cplusplus
extern "C" {
#endif

/* Bind distinct, non-overlapping blocks before first use. Failed validation
 * leaves all blocks unchanged. Their lifetimes cover the entire core session. */
ksn_result ksn_core_bind(ksn_core *,ksn_core_command_block *,ksn_core_command_block *,
                         ksn_core_text_block *,ksn_core_text_block *);
/* Reset an already bound core, preserving borrowed block addresses.
 * Reinitialization invalidates handles; destroy guest callbacks first.
 * All core instances share one owner task and process-lifetime ID counters
 * (12 bytes outside this storage, included conservatively in each core's
 * limits/stats). Storage must not be copied or relocated. */
void ksn_core_init(ksn_core *core);
ksn_client ksn_core_client(ksn_core *core,ksn_layer layer);
/* Host-only append-only registration. Providers stay immutable/alive until
 * init, which must run outside provider callbacks and invalidate all clients.
 * Register only between submissions/builders. No per-frame retain/release. */
ksn_result ksn_core_register_image(ksn_core *core,ksn_layer layer,const ksn_image_port *port,ksn_resource *out);

/* Host-only, synchronous owner-task interface. A submission ticket validates
 * every read/ack, including across discard and reinitialization. No bank
 * pointers escape. Read one command at a time into reusable caller storage. */
typedef struct {
    ksn_tx ticket;
    ksn_capacity previous[2],next[2];
    ksn_rgba previous_background,next_background;
    bool full_redraw;
} ksn_frame;
typedef struct {
    ksn_draw draw;
    bool visible;
    uint8_t reveal;
    bool group_begin,group_end;
    uint8_t group_opacity;
    char text[128]; /* Counted UTF-8, not NUL terminated. draw points here. */
} ksn_frame_command;
bool ksn_core_has_submission(const ksn_core *core);
/* Last submission only, retained across begin/abort. Poll before next end. */
ksn_submission ksn_core_poll(const ksn_core *core);
bool ksn_core_needs_repair(const ksn_core *core);
/* Owner invalidation is independent of guest submissions. Requests received
 * during a transfer remain pending until a subsequent complete frame. */
void ksn_core_invalidate(ksn_core *core);
ksn_result ksn_core_discard_reason(ksn_core *core,ksn_tx ticket,ksn_result reason);
ksn_result ksn_core_check_builder(const ksn_core *core,ksn_tx ticket,ksn_layer layer,ksn_update_mode mode);
ksn_result ksn_core_builder_usage(const ksn_core *core,ksn_tx ticket,ksn_capacity *out);
/* One isolated group over a consecutive range; no overlaps/nesting. PATCH
 * may change opacity only on the exact existing range. Owner-task API. */
ksn_result ksn_core_group(ksn_core *core,ksn_layer layer,ksn_tx tx,ksn_ref first,
                        uint16_t count,uint8_t opacity);
ksn_result ksn_core_frame(const ksn_core *core,ksn_frame *out);
/* Renderer-only start: consume the current invalidation request. If no guest
 * submission exists, pin the committed bank with a private repair token.
 * Repair never changes reference generations or the last guest outcome. */
ksn_result ksn_core_prepare_frame(ksn_core *core,ksn_frame *out);
/* Renderer preflight failed before any transfer. Release only a private
 * repair pin, keeping invalidation pending; guest submissions stay sealed. */
void ksn_core_defer_repair(ksn_core *core,ksn_tx ticket);
ksn_result ksn_core_read(const ksn_core *core,ksn_tx ticket,bool previous,
                       ksn_layer layer,uint16_t index,ksn_frame_command *out);
ksn_result ksn_core_image_span(const ksn_core *core,ksn_tx ticket,bool previous,
                            ksn_layer layer,uint16_t index,uint16_t y,uint16_t x,
                            uint16_t count,uint16_t *rgb565,uint8_t *alpha);
/* Cardputer's 17 full-width bands, last one 7 rows. No state mutation. */
ksn_result ksn_core_damage(const ksn_core *core,ksn_tx ticket,uint32_t *bands);
/* Call failed on any partial/uncertain LCD transfer, before retry or discard.
 * presented attests that all required bands were transferred successfully. */
ksn_result ksn_core_failed(ksn_core *core,ksn_tx ticket);
ksn_result ksn_core_presented(ksn_core *core,ksn_tx ticket);
ksn_result ksn_core_discard(ksn_core *core,ksn_tx ticket);
ksn_capacity ksn_core_active_usage(const ksn_core *core,ksn_layer layer);
ksn_capacity ksn_core_submission_usage(const ksn_core *core,ksn_layer layer);
/* Host/cache bookkeeping only. Validates a consecutive range against the
 * displayed bank without exposing that bank. */
bool ksn_core_refs_active(const ksn_core *core,ksn_layer layer,ksn_ref first,uint16_t count);

#ifdef __cplusplus
}
#endif
#endif
