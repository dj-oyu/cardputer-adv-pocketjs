#ifndef KSN_CORE_H
#define KSN_CORE_H
#include "ksn_api.h"
#include "ksn_ports.h"
#include "ksn_composition_types.h"
#define KSN_RESOURCES 16u
#define KSN_APP_TRACKS 6u
#define KSN_SYSTEM_TRACKS 2u
#define KSN_TRACKS (KSN_APP_TRACKS+KSN_SYSTEM_TRACKS)
typedef struct {
    uint64_t started_us,sampled_us;
    ksn_pose from,to;
    ksn_animation id;ksn_ref target;
    uint32_t duration_ms;
    uint8_t easing,repeat,status,pad;
} ksn_track;
typedef struct { ksn_track tracks[KSN_TRACKS]; } ksn_core_animation_block;

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
    ksn_track *tracks;
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
    uint64_t animation_now_us;
    ksn_tx transaction;
    ksn_submission outcome;
    ksn_result poison;
    ksn_layer layer;
    ksn_update_mode mode;
    uint8_t active,building_bank;
    /* Two band sets, not one flag: `invalidated` is what an owner has asked for
     * and prepare_frame has not taken yet, `repair_bands` is what the frame in
     * flight owes. An unqualified invalidate sets every bit in both and so is
     * the flag it used to be; a qualified one names the rows its overlay
     * occupies, which is the difference between a keystroke repainting the
     * field and a keystroke repainting the screen. */
    uint32_t invalidated,repair_bands;
    bool building,submitted,full_redraw,repairing;
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
/* Owner-only teardown outside rendering. Cancel this layer's builder/submission
 * first. Clear both banks and image providers; preserve the other layer, even
 * when it has pending work. Forces a full repaint without consuming an ID. */
ksn_result ksn_core_reset_layer(ksn_core *,ksn_layer);
ksn_client ksn_core_client(ksn_core *core,ksn_layer layer);
/* Host-only append-only registration. Providers stay immutable/alive until
 * init, which must run outside provider callbacks and invalidate all clients.
 * Register only between submissions/builders. No per-frame retain/release. */
ksn_result ksn_core_register_image(ksn_core *core,ksn_layer layer,const ksn_image_port *port,ksn_resource *out);
/* Read-only owner-turn validation against currently registered resources. */
ksn_result ksn_core_check_draw(const ksn_core *,ksn_layer,const ksn_draw *);
/* Optional caller-owned blocks, attached before the first animation. */
ksn_result ksn_core_enable_animation(ksn_core *,ksn_core_animation_block *,ksn_core_animation_block *);
ksn_result ksn_core_finish_animation(ksn_core *,ksn_layer,ksn_tx,ksn_animation);
ksn_animation_status ksn_core_poll_animation(const ksn_core *,ksn_layer,ksn_animation);
void ksn_core_start_animations(ksn_core *,uint64_t presented_us);
void ksn_core_set_animation_time(ksn_core *,uint64_t now_us);
uint64_t ksn_core_animation_deadline(const ksn_core *);
ksn_result ksn_core_advance_animations(ksn_core *,uint64_t now_us,bool reduce_motion,ksn_tx *out);
uint32_t ksn_core_animation_bytes(const ksn_core *);

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
/* The same request, limited to the 8-row bands named in `bands` (bit b is rows
 * 8b..8b+7, bit 16 the last seven). Bits outside the panel are ignored and an
 * empty set is a no-op. For an owner that composites over the strip and knows
 * which rows it touched -- the text field of pocket_text.c is the one this
 * exists for -- so that its damage costs what it covers. Unqualified
 * invalidation stays available for owners that do not know. */
#define KSN_BANDS_ALL ((1u<<17)-1u)
void ksn_core_invalidate_bands(ksn_core *core,uint32_t bands);

/* What a frame owes the panel: the bands, and for each of them the half-open
 * column range inside it. The columns are the union of the changed commands'
 * clipped boxes on that band -- a counter that redraws six pixels of a caption
 * dirties six columns, not two hundred and forty. Bands not in `bands` have no
 * meaningful x0/x1. Repair and full-redraw bands carry [0,240) because the
 * owner that asked for them does not describe columns. */
typedef struct {
    uint32_t bands;
    int16_t x0[17],x1[17];
} ksn_damage;
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
/* Owner-only read of one committed reference. Text points directly into the
 * active bank and is read-only until the next successful presentation/reset.
 * Never retain the pointer or call from a display callback. */
ksn_result ksn_core_read_active_ref(const ksn_core *core,ksn_layer layer,
                                    ksn_ref ref,ksn_frame_command *out);
ksn_result ksn_core_image_span(const ksn_core *core,ksn_tx ticket,bool previous,
                            ksn_layer layer,uint16_t index,uint16_t y,uint16_t x,
                            uint16_t count,uint16_t *rgb565,uint8_t *alpha);
/* Cardputer's 17 bands, last one 7 rows, each with its column range. No state
 * mutation. A caller that cannot transfer a partial row widens every band to
 * [0,240) itself before compositing -- the renderer's narrow arm has to be a
 * decision made before pixels are written, not after. */
ksn_result ksn_core_damage(const ksn_core *core,ksn_tx ticket,
                           const ksn_text_port *text,ksn_damage *out);
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
