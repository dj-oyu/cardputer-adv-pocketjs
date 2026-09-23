#ifndef KSN_SOURCE_H
#define KSN_SOURCE_H
#include "ksn_schema.h"

/* App/service-owned capability registry. Kasane knows neither source names nor
 * domains. A snapshot is complete, immutable until release, and borrowed only
 * for the owner turn. The producer must not wait for a UI consumer. */
#define KSN_SOURCE_ABI_VERSION 1u
#define KSN_SOURCE_MAX_REGISTERED 4u
#define KSN_SOURCE_MAX_FIELDS KSN_SCHEMA_MAX_SLOTS

typedef struct {
    uint8_t index;
    uint32_t generation;
} ksn_source_handle;

typedef struct {
    uint16_t size,version;
    uint8_t field_count;
    uint32_t generation;
    uint64_t revision,expires_at_us;
    uint32_t valid_fields,changed_fields;
    const ksn_schema_value *fields;
} ksn_source_snapshot;

typedef struct {
    uint16_t size,version;
    uint8_t field_count;
    const ksn_slot_type *field_types;
    void *context;
    ksn_result (*acquire)(void *context,uint64_t cursor,uint64_t now_us,
                          ksn_source_snapshot *out);
    void (*release)(void *context,const ksn_source_snapshot *snapshot);
    bool (*allow)(void *context,uint32_t consumer);
} ksn_source_provider;

typedef struct {
    const ksn_source_provider *provider;
    uint32_t generation,pins;
} ksn_source_entry;
typedef struct {
    ksn_source_entry entries[KSN_SOURCE_MAX_REGISTERED];
} ksn_source_registry;
typedef struct {uint8_t slot,field;} ksn_source_binding;
typedef struct {
    ksn_source_handle handle;
    uint32_t consumer,bound_slots,valid_slots;
    uint64_t validated_revision,displayed_revision;
    uint8_t binding_count;
    bool has_validated;
    ksn_source_binding bindings[KSN_SOURCE_MAX_FIELDS];
} ksn_source_subscription;
typedef struct {
    ksn_source_registry *registry;
    ksn_source_subscription *subscription;
    const ksn_source_provider *provider;
    ksn_source_snapshot snapshot;
    uint32_t valid_slots,dirty_slots;
    bool active,committed;
} ksn_source_lease;

void ksn_source_registry_init(ksn_source_registry *registry);
ksn_result ksn_source_register(ksn_source_registry *registry,
                               const ksn_source_provider *provider,
                               ksn_source_handle *out);
ksn_result ksn_source_unregister(ksn_source_registry *registry,
                                 ksn_source_handle handle);
ksn_result ksn_source_subscribe(ksn_source_registry *registry,
                                ksn_source_handle handle,uint32_t consumer,
                                const ksn_schema *schema,
                                const ksn_source_binding *bindings,uint8_t count,
                                ksn_source_subscription *out);
/* effective is metadata only; text points into the pinned snapshot. Caller
 * validates/preflights/submits before release and never retains these pointers. */
ksn_result ksn_source_acquire(ksn_source_registry *registry,
                              ksn_source_subscription *subscription,
                              const ksn_schema *schema,
                              const ksn_schema_value *base,uint64_t now_us,
                              ksn_schema_value effective[KSN_SCHEMA_MAX_SLOTS],
                              ksn_source_lease *lease);
/* Advance the read cursor only after the caller has accepted a complete,
 * validated effective value set. A failed preflight must not call commit. */
ksn_result ksn_source_commit(ksn_source_lease *lease);
void ksn_source_release(ksn_source_lease *lease);
ksn_result ksn_source_presented(ksn_source_subscription *subscription,
                                uint64_t revision);
#endif
