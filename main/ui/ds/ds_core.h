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

/* Caller-owned, fixed storage. The implementation performs no heap allocation. */
typedef union {
    max_align_t alignment;
    uint8_t bytes[DS_CORE_STORAGE_BYTES];
} ds_core;

#ifdef __cplusplus
extern "C" {
#endif

void ds_core_init(ds_core *core);
ds_client ds_core_client(ds_core *core,ds_layer layer);

/* end() creates one submission. Rendering inspects it through a private
 * backend interface added with the compositor. Until then no new begin() is
 * accepted. presented() makes it the patch baseline; discard keeps the old
 * baseline. Both are host-owner operations, never guest API. */
bool ds_core_has_submission(const ds_core *core);
ds_result ds_core_presented(ds_core *core);
ds_result ds_core_discard(ds_core *core);
ds_capacity ds_core_active_usage(const ds_core *core,ds_layer layer);
ds_capacity ds_core_submission_usage(const ds_core *core,ds_layer layer);

#ifdef __cplusplus
}
#endif
#endif
