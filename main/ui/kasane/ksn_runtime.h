#ifndef KSN_RUNTIME_H
#define KSN_RUNTIME_H
#include "ksn_view_host.h"

/* One native owner task, independent of QuickJS. Keep leases by value and
 * resolve them for EACH operation. Returned views are borrowed only until the
 * next lifecycle call; never retain a view across detach/shutdown. */
typedef struct { uint32_t value; } ksn_app_lease;
#ifdef __cplusplus
extern "C" {
#endif
ksn_result ksn_runtime_app_attach(ksn_app_lease *);
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
ksn_result ksn_runtime_present(const ksn_display_port *,ksn_render_stats *);
ksn_input_scope ksn_runtime_input_scope(bool host_priority);
#ifdef __cplusplus
}
#endif
#endif
