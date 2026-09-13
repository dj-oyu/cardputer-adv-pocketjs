#ifndef DS_CORE_H
#define DS_CORE_H
#include "ds_api.h"

#define DS_APP_COMMANDS 80u
#define DS_SYSTEM_COMMANDS 16u
#define DS_COMMANDS (DS_APP_COMMANDS + DS_SYSTEM_COMMANDS)
#define DS_APP_TEXT_BYTES 896u
#define DS_SYSTEM_TEXT_BYTES 128u
#define DS_TEXT_BYTES (DS_APP_TEXT_BYTES + DS_SYSTEM_TEXT_BYTES)
#define DS_CORE_STORAGE_BYTES 9216u

/* Host storage layout, not a guest API. Do not access fields directly.
 * A typed member avoids accessing a declared byte array as an unrelated struct. */
typedef struct ds_core_impl ds_core_impl;
typedef struct { ds_core_impl *core; ds_layer layer; } ds_endpoint;
typedef struct {
    ds_command_storage commands[DS_COMMANDS];
    uint8_t text[DS_TEXT_BYTES];
    uint16_t count[2],text_used[2];
    uint32_t generation[2];
    ds_rgba background[2];
    bool background_set[2];
} ds_bank;
struct ds_core_impl {
    ds_bank banks[2];
    ds_endpoint endpoints[2];
    ds_tx transaction;
    ds_result poison;
    ds_layer layer;
    ds_update_mode mode;
    uint8_t active,building_bank;
    bool building,submitted,full_redraw;
};

/* Caller-owned, fixed storage. The implementation performs no heap allocation. */
typedef union {
    max_align_t alignment;
    ds_core_impl state;
    uint8_t bytes[DS_CORE_STORAGE_BYTES];
} ds_core;

#ifdef __cplusplus
extern "C" {
#endif

/* Reinitialization invalidates handles; destroy guest callbacks first.
 * All core instances share one owner task and process-lifetime ID counters
 * (8 bytes outside this storage, included conservatively in each core's
 * limits/stats). Storage must not be copied or relocated. */
void ds_core_init(ds_core *core);
ds_client ds_core_client(ds_core *core,ds_layer layer);

/* Host-only, synchronous owner-task interface. A submission ticket validates
 * every read/ack, including across discard and reinitialization. No bank
 * pointers escape. Read one command at a time into reusable caller storage. */
typedef struct {
    ds_tx ticket;
    ds_capacity previous[2],next[2];
    ds_rgba previous_background,next_background;
    bool full_redraw;
} ds_frame;
typedef struct {
    ds_draw draw;
    bool visible;
    uint8_t reveal;
    char text[128]; /* Counted UTF-8, not NUL terminated. draw points here. */
} ds_frame_command;
bool ds_core_has_submission(const ds_core *core);
ds_result ds_core_frame(const ds_core *core,ds_frame *out);
ds_result ds_core_read(const ds_core *core,ds_tx ticket,bool previous,
                       ds_layer layer,uint16_t index,ds_frame_command *out);
/* Call failed on any partial/uncertain LCD transfer, before retry or discard.
 * presented attests that all required bands were transferred successfully. */
ds_result ds_core_failed(ds_core *core,ds_tx ticket);
ds_result ds_core_presented(ds_core *core,ds_tx ticket);
ds_result ds_core_discard(ds_core *core,ds_tx ticket);
ds_capacity ds_core_active_usage(const ds_core *core,ds_layer layer);
ds_capacity ds_core_submission_usage(const ds_core *core,ds_layer layer);

#ifdef __cplusplus
}
#endif
#endif
