#ifndef KSN_RUNTIME_H
#define KSN_RUNTIME_H
#include "ksn_view_host.h"

/* One native owner task, independent of QuickJS. Keep leases by value and
 * resolve them for EACH operation. Returned views are borrowed only until the
 * next lifecycle call; never retain a view across detach/shutdown. */
typedef struct { uint32_t value; } ksn_app_lease;
/* The runtime is one allocation: control state, both banks, and optionally
 * the APP owner's own state behind them (attach_tail). */
#define KSN_RUNTIME_BASE_BUDGET (KSN_CORE_STORAGE_BYTES+1024u)
#define KSN_RUNTIME_TAIL_BUDGET 3072u
#define KSN_RUNTIME_TAIL_ALIGN 8u
#ifdef __cplusplus
extern "C" {
#endif
ksn_result ksn_runtime_app_attach(ksn_app_lease *);
/* As attach, and also hands out `bytes` of zeroed storage for the APP owner.
 * When this call creates the runtime the storage is inside its one block;
 * beside an existing SYSTEM owner it is a separate allocation. Either way the
 * runtime owns it: it is gone after a successful detach. */
ksn_result ksn_runtime_app_attach_tail(ksn_app_lease *,uint32_t bytes,void **tail);
ksn_result ksn_runtime_app_detach(ksn_app_lease);
ksn_view *ksn_runtime_app_view(ksn_app_lease);
/* APP-lifetime SYSTEM compositor. Does not pin storage or replace an explicit
 * native SYSTEM owner. Resolve again after every lifecycle operation. */
ksn_view *ksn_runtime_app_system_view(ksn_app_lease);
void ksn_runtime_app_end_turn(ksn_app_lease);
void ksn_runtime_app_activate(ksn_app_lease);
/* Acquire pins native storage beyond guest lifetimes. The caller pumps native
 * presentation even without a guest. Shutdown requires no APP or pending work. */
ksn_result ksn_runtime_system_acquire(ksn_view **);
ksn_result ksn_runtime_shutdown(void);
ksn_result ksn_runtime_cache_create(ksn_view *,const ksn_draw *,uint16_t,ksn_template *);
ksn_result ksn_runtime_animate(ksn_view *,ksn_tx,const ksn_motion *,ksn_animation *);
ksn_result ksn_runtime_advance_animations(uint64_t now_us);
void ksn_runtime_animations_presented(uint64_t now_us);
void ksn_runtime_set_animation_time(uint64_t now_us);
uint64_t ksn_runtime_animation_deadline(void);
bool ksn_runtime_animation_pending(void);
void ksn_runtime_set_hidden(bool hidden);
void ksn_runtime_set_reduce_motion(bool enabled);
uint32_t ksn_runtime_reserved_bytes(void);
uint32_t ksn_runtime_cache_bytes(void);
ksn_view_stats ksn_runtime_stats(ksn_layer);
bool ksn_runtime_has_submission(void);
bool ksn_runtime_needs_present(void);
void ksn_runtime_invalidate(void);
void ksn_runtime_invalidate_bands(uint32_t bands);
ksn_result ksn_runtime_present(const ksn_display_port *,ksn_render_stats *);
ksn_result ksn_runtime_present_backdrop(const ksn_display_port *,ksn_backdrop_loader,
                                        ksn_render_stats *);
ksn_input_scope ksn_runtime_input_scope(bool host_priority);
#ifdef __cplusplus
}
#endif
#endif
