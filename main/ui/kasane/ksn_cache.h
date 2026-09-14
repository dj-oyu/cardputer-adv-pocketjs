#ifndef KSN_CACHE_H
#define KSN_CACHE_H
#include "ksn_core.h"

#define KSN_CACHE_TEMPLATES 8u
#define KSN_CACHE_INSTANCES 8u
#define KSN_CACHE_COMMANDS 48u
#define KSN_CACHE_TEXT_BYTES 1024u
#define KSN_CACHE_STORAGE_BYTES 4096u

typedef struct {
    uint32_t id;
    uint16_t first_command,text_offset,text_bytes;
    uint8_t command_count,layer;
} ksn_cache_template_entry;
typedef struct {
    uint32_t id,template_id,pending_tx;
    ksn_ref first;
    ksn_placement current,pending;
    uint8_t command_count,layer,flags;
} ksn_cache_instance_entry;
typedef struct {
    ksn_command_storage commands[KSN_CACHE_COMMANDS];
    uint8_t text[KSN_CACHE_TEXT_BYTES];
    ksn_cache_template_entry templates[KSN_CACHE_TEMPLATES];
    ksn_cache_instance_entry instances[KSN_CACHE_INSTANCES];
    uint16_t command_used,text_used;
    uint8_t template_count,instance_count;
} ksn_cache_impl;
typedef union {
    max_align_t alignment;
    ksn_cache_impl state;
    uint8_t bytes[KSN_CACHE_STORAGE_BYTES];
} ksn_cache;
#ifdef __cplusplus
extern "C" {
#endif
void ksn_cache_init(ksn_cache *cache);
/* v0.2 initial subset: RECT, ROUND_RECT and STROKE. Definitions are copied. */
ksn_result ksn_cache_create(ksn_cache *cache,ksn_layer layer,const ksn_draw *draws,
                          uint16_t count,ksn_template *out);
ksn_result ksn_cache_release(ksn_cache *cache,ksn_template handle);
/* Instantiate only in REPLACE. Every instance uses isolated premultiplied
 * composition, including opacity 255, to keep rounding stable under PATCH. */
ksn_result ksn_cache_instantiate(ksn_cache *cache,ksn_core *core,ksn_tx tx,
                               ksn_template handle,const ksn_placement *placement,
                               ksn_instance *out);
ksn_result ksn_cache_place(ksn_cache *cache,ksn_core *core,ksn_tx tx,
                         ksn_instance handle,const ksn_placement *placement);
ksn_result ksn_cache_set_visible(ksn_cache *cache,ksn_core *core,ksn_tx tx,
                               ksn_instance handle,bool visible);
/* Pair with ksn_api.abort for a transaction that touched cache instances. */
ksn_result ksn_cache_abort(ksn_cache *cache,ksn_tx ticket);
/* Call immediately after core presented/discarded. presented=true requires the
 * new displayed bank to be installed already. Omitted instances are detached. */
ksn_result ksn_cache_resolve(ksn_cache *cache,const ksn_core *core,ksn_tx ticket,bool presented);
ksn_cache_stats ksn_cache_get_stats(const ksn_cache *cache);
#ifdef __cplusplus
}
#endif
#endif
