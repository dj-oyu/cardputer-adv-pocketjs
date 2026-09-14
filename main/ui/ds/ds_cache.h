#ifndef DS_CACHE_H
#define DS_CACHE_H
#include "ds_core.h"

#define DS_CACHE_TEMPLATES 8u
#define DS_CACHE_INSTANCES 8u
#define DS_CACHE_COMMANDS 48u
#define DS_CACHE_TEXT_BYTES 1024u
#define DS_CACHE_STORAGE_BYTES 4096u

typedef struct {
    uint32_t id;
    uint16_t first_command,text_offset,text_bytes;
    uint8_t command_count,layer;
} ds_cache_template_entry;
typedef struct {
    uint32_t id,template_id,pending_tx;
    ds_ref first;
    ds_placement current,pending;
    uint8_t command_count,layer,flags;
} ds_cache_instance_entry;
typedef struct {
    ds_command_storage commands[DS_CACHE_COMMANDS];
    uint8_t text[DS_CACHE_TEXT_BYTES];
    ds_cache_template_entry templates[DS_CACHE_TEMPLATES];
    ds_cache_instance_entry instances[DS_CACHE_INSTANCES];
    uint16_t command_used,text_used;
    uint8_t template_count,instance_count;
} ds_cache_impl;
typedef union {
    max_align_t alignment;
    ds_cache_impl state;
    uint8_t bytes[DS_CACHE_STORAGE_BYTES];
} ds_cache;
#ifdef __cplusplus
extern "C" {
#endif
void ds_cache_init(ds_cache *cache);
/* v0.2 initial subset: RECT, ROUND_RECT and STROKE. Definitions are copied. */
ds_result ds_cache_create(ds_cache *cache,ds_layer layer,const ds_draw *draws,
                          uint16_t count,ds_template *out);
ds_result ds_cache_release(ds_cache *cache,ds_template handle);
/* Instantiate only in REPLACE. Every instance uses isolated premultiplied
 * composition, including opacity 255, to keep rounding stable under PATCH. */
ds_result ds_cache_instantiate(ds_cache *cache,ds_core *core,ds_tx tx,
                               ds_template handle,const ds_placement *placement,
                               ds_instance *out);
ds_result ds_cache_place(ds_cache *cache,ds_core *core,ds_tx tx,
                         ds_instance handle,const ds_placement *placement);
ds_result ds_cache_set_visible(ds_cache *cache,ds_core *core,ds_tx tx,
                               ds_instance handle,bool visible);
/* Pair with ds_api.abort for a transaction that touched cache instances. */
ds_result ds_cache_abort(ds_cache *cache,ds_tx ticket);
/* Call immediately after core presented/discarded. presented=true requires the
 * new displayed bank to be installed already. Omitted instances are detached. */
ds_result ds_cache_resolve(ds_cache *cache,const ds_core *core,ds_tx ticket,bool presented);
ds_cache_stats ds_cache_get_stats(const ds_cache *cache);
#ifdef __cplusplus
}
#endif
#endif
