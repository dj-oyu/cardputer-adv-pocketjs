#ifndef KSN_SOURCE_H
#define KSN_SOURCE_H
#include "ksn_schema.h"

/* App/service-owned capability registry. Kasane knows neither source names nor
 * domains. A snapshot is complete, immutable until release, and borrowed only
 * for the owner turn. The producer must not wait for a UI consumer. */
#define KSN_SOURCE_ABI_VERSION 2u
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
    bool has_validated,identity_bindings;
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
typedef struct {
    ksn_source_registry *registry;
    ksn_source_subscription *subscription;
} ksn_source_member;
typedef struct {
    ksn_source_lease leases[KSN_SOURCE_MAX_REGISTERED];
    uint32_t dirty_slots;
    uint8_t count;
    bool active;
} ksn_source_bundle;

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
/* effective is metadata only, distinct from base; text points into the pinned
 * snapshot. On a malformed snapshot it is restored to base (or remains
 * untouched if acquisition failed before composition). Caller
 * validates/preflights/submits before release and never retains these pointers. */
ksn_result ksn_source_acquire(ksn_source_registry *registry,
                              ksn_source_subscription *subscription,
                              const ksn_schema *schema,
                              const ksn_schema_value *base,uint64_t now_us,
                              ksn_schema_value effective[KSN_SCHEMA_MAX_SLOTS],
                              ksn_source_lease *lease);
/* Custom native projections can borrow the complete producer snapshot after
 * subscribe validated their immutable schema and bindings. No base/effective
 * metadata copy or schema walk is performed. The caller must honor
 * lease.valid_slots, validate domain constraints before commit, and never
 * retain snapshot.fields after release. */
ksn_result ksn_source_borrow(ksn_source_registry *registry,
                             ksn_source_subscription *subscription,
                             uint64_t now_us,ksn_source_lease *lease);
/* Native owner-turn fast path for an unchanged, subscribe-validated identity
 * mapping (slot i <- field i). It still checks capability generation, policy,
 * snapshot shape and cursor, pins storage, and computes validity/dirty masks.
 * The caller must not mutate subscription bindings after subscribe. */
ksn_result ksn_source_borrow_identity(ksn_source_registry *registry,
                                      ksn_source_subscription *subscription,
                                      uint64_t now_us,ksn_source_lease *lease);
#ifdef KASANE_P0_PROBE
void ksn_source_borrow_probe_report(void);
#endif
/* Compose disjoint subscriptions from one or more registries with one
 * base->effective metadata copy and no payload copy. All leases remain pinned
 * until bundle_release. Initialize bundle to zero before first use. A failure
 * after composition begins releases every pin, restores effective to base,
 * and advances no source cursor. */
ksn_result ksn_source_bundle_acquire(const ksn_source_member *members,uint8_t count,
    const ksn_schema *schema,const ksn_schema_value *base,uint64_t now_us,
    ksn_schema_value effective[KSN_SCHEMA_MAX_SLOTS],ksn_source_bundle *bundle);
ksn_result ksn_source_bundle_commit(ksn_source_bundle *bundle);
void ksn_source_bundle_release(ksn_source_bundle *bundle);
/* Advance the read cursor only after the caller has accepted a complete,
 * validated effective value set. A failed preflight must not call commit. */
ksn_result ksn_source_commit(ksn_source_lease *lease);
void ksn_source_release(ksn_source_lease *lease);
ksn_result ksn_source_presented(ksn_source_subscription *subscription,
                                uint64_t revision);
#endif
