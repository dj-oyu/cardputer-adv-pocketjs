#ifndef KSN_SCHEMA_SESSION_H
#define KSN_SCHEMA_SESSION_H
#include "ksn_schema.h"

/* Caller-owned, app-independent mounted state. It borrows both immutable
 * definitions and each turn's values; neither is copied or retained. */
typedef struct {
    const ksn_schema *schema;
    ksn_schema_dependencies dependencies;
    ksn_schema_ref_map active_map,candidate_map;
    ksn_ref active_refs[KSN_SCHEMA_MAX_NODES*2u];
    ksn_ref candidate_refs[KSN_SCHEMA_MAX_NODES*2u];
    ksn_rect active_viewport,candidate_viewport;
    ksn_rgba active_background,candidate_background;
    ksn_tx ticket;
    uint64_t applied_revision,pending_revision;
    uint32_t pending_dirty,inflight_dirty;
    uint8_t active_count,candidate_count;
    ksn_schema_delta pending_delta;
    bool has_active,has_map,candidate_map_valid,dirty_unknown,inflight_unknown;
} ksn_schema_session;

ksn_result ksn_schema_session_init(ksn_schema_session *session,
                                   const ksn_schema *schema);
/* Consume a terminal display outcome without resolving/submitting new work.
 * An overlay can then run its guest before a fast native source submits again. */
ksn_result ksn_schema_session_settle(ksn_schema_session *session,ksn_view *view,
                                     bool *blocked);
/* revision is monotonic for changed source values. Equal revision and viewport
 * bypass all schema work. Pending updates coalesce into the latest values
 * supplied on a later owner turn; no pointer survives this call. */
ksn_result ksn_schema_session_step(ksn_schema_session *session,ksn_view *view,
                                   ksn_rect viewport,const ksn_schema_value *values,
                                   uint64_t revision,bool *blocked);
/* dirty_slots covers every base/source field changed since the last accepted
 * step. Later calls may add bits while a ticket is submitted; a discarded
 * ticket restores its inflight bits. The legacy step remains the full-scan
 * reference for callers that cannot supply an exact mask. */
ksn_result ksn_schema_session_step_dirty(ksn_schema_session *session,ksn_view *view,
    ksn_rect viewport,const ksn_schema_value *values,uint64_t revision,
    uint32_t dirty_slots,bool *blocked);
#ifdef KASANE_P0_PROBE
/* Diagnostic-only same-image A/B selector. Production has no branch or state. */
bool ksn_schema_session_toggle_fullscan_probe(void);
#endif
#endif
