#ifndef KSN_SOURCE_POOL_ADAPTER_H
#define KSN_SOURCE_POOL_ADAPTER_H
#include "ksn_source.h"
#include "ksn_source_pool.h"

/* A pool payload is application-owned. describe borrows its complete current
 * field array and metadata; it must not allocate, mutate, or retain pointers.
 * The field array must start at fields_offset within every payload slot. */
typedef struct {
    const ksn_schema_value *fields;
    uint64_t expires_at_us;
    uint32_t valid_fields,changed_fields;
} ksn_source_pool_view;
typedef ksn_result (*ksn_source_pool_describe)(const void *payload,
                                               ksn_source_pool_view *out);
typedef struct {
    ksn_source_pool *pool;
    const ksn_slot_type *field_types;
    ksn_source_pool_describe describe;
    bool (*allow)(void *policy,uint32_t consumer);
    void *policy;
    size_t fields_offset;
    uint32_t generation;
    uint8_t field_count;
} ksn_source_pool_adapter;

/* Configure before registration, then bind the returned handle's generation.
 * A producer may publish on another task; registry/subscription calls remain
 * on the UI owner task. Keep adapter, pool, types and payloads alive until all
 * source leases have been released and the provider is unregistered. */
ksn_result ksn_source_pool_adapter_open(ksn_source_pool_adapter *adapter,
    ksn_source_pool *pool,const ksn_slot_type *field_types,uint8_t field_count,
    size_t fields_offset,ksn_source_pool_describe describe,
    bool (*allow)(void *policy,uint32_t consumer),void *policy,
    ksn_source_provider *provider);
ksn_result ksn_source_pool_adapter_registered(ksn_source_pool_adapter *adapter,
                                               ksn_source_handle handle);
#endif
