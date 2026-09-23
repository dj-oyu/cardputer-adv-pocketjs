#ifndef KSN_SCHEMA_SESSION_H
#define KSN_SCHEMA_SESSION_H
#include "ksn_schema.h"

/* Caller-owned, app-independent mounted state. It borrows both immutable
 * definitions and each turn's values; neither is copied or retained. */
typedef struct {
    const ksn_schema *schema;
    ksn_schema_dependencies dependencies;
    ksn_ref active_refs[KSN_SCHEMA_MAX_NODES*2u];
    ksn_ref candidate_refs[KSN_SCHEMA_MAX_NODES*2u];
    ksn_rect active_viewport,candidate_viewport;
    ksn_rgba active_background,candidate_background;
    ksn_tx ticket;
    uint64_t applied_revision,pending_revision;
    uint8_t active_count,candidate_count;
    ksn_schema_delta pending_delta;
    bool has_active;
} ksn_schema_session;

ksn_result ksn_schema_session_init(ksn_schema_session *session,
                                   const ksn_schema *schema);
/* revision is monotonic for changed source values. Equal revision and viewport
 * bypass all schema work. Pending updates coalesce into the latest values
 * supplied on a later owner turn; no pointer survives this call. */
ksn_result ksn_schema_session_step(ksn_schema_session *session,ksn_view *view,
                                   ksn_rect viewport,const ksn_schema_value *values,
                                   uint64_t revision,bool *blocked);
#endif
