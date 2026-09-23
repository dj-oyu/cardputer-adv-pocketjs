#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "quickjs.h"
#include "ui/kasane/ksn_view_host.h"
#include "ui/kasane/ksn_source.h"
#include "system/sys_notify.h"

/* QuickJS-facing APP lease into the native Kasane runtime. Allocation is lazy
 * (first Kasane call, or pocket_kasane_prepare); reading features costs no arena. Reset detaches APP and releases its wrapper
 * bookkeeping; a native SYSTEM owner keeps the shared storage alive.
 * Owner task only, reset outside guest/render callbacks. */
esp_err_t pocket_kasane_install(JSContext *ctx, void *user_data);
/* C service publishes a session-scoped, unforgeable source capability. The
 * registry and provider must outlive pocket_kasane_reset(); revoke by
 * unregistering the handle, which makes later binds/acquires stale. */
JSValue pocket_kasane_source_capability(JSContext *ctx,ksn_source_registry *registry,
                                         ksn_source_handle handle);
void pocket_kasane_reset(void);
/* Takes the native arena now, as one block, instead of at the guest's first
 * Kasane call. The session calls it before evaluating a source that names
 * kasane, while the heap is still unbroken. Failure is silent: the first call
 * then retries and reports OUT_OF_MEMORY where the app can see it. */
void pocket_kasane_prepare(void);
/* Bind the JS coordinate space to a host-owned viewport. The ordinary app
 * viewport is the whole panel. Overlay coordinates are region-local: direct
 * destinations are translated immediately; cache templates remain local and
 * are translated/clipped at placement. Configure before source evaluation;
 * reset restores the whole panel and normal feature profile. */
void pocket_kasane_set_viewport(int16_t x,int16_t y,int16_t width,int16_t height);

bool pocket_kasane_active(void);
/* Advances the native presenter's acknowledged/queued plans in the owner turn.
 * blocked means it submitted a new APP bank, so guest frame must wait. */
ksn_result pocket_kasane_presenter_step(bool *blocked);
/* Host-owned transient status slot. It overrides the app message without
 * mutating it; expiry reveals the latest app/playback value on the next turn. */
ksn_result pocket_kasane_presenter_host_status(const char *text,size_t bytes,uint64_t until_us);
ksn_result pocket_kasane_update_notice(const sys_notice *,uint16_t variant);
bool pocket_kasane_notice_composited(void);
bool pocket_kasane_system_pending(void);
bool pocket_kasane_has_submission(void);
/* Bound the owner wait by the next native-source expiry. A due/failed source
 * is retried by the regular frame, so it cannot create a zero-tick spin. */
uint32_t pocket_kasane_source_wait_ticks(uint64_t now_us,uint32_t cap,uint32_t hz);
ksn_result pocket_kasane_advance(uint64_t now_us);
bool pocket_kasane_animation_pending(void);
void pocket_kasane_animations_presented(uint64_t now_us);
void pocket_kasane_set_animation_time(uint64_t now_us);
bool pocket_kasane_needs_present(void);
void pocket_kasane_invalidate(void);
// The same, for an owner that knows which 8-row bands its overlay covers.
void pocket_kasane_invalidate_bands(uint32_t bands);
ksn_result pocket_kasane_present(const ksn_display_port *display,
                                 ksn_render_stats *stats);
ksn_result pocket_kasane_present_backdrop(const ksn_display_port *,ksn_backdrop_loader,
                                          ksn_render_stats *);
void pocket_kasane_end_turn(void);
ksn_input_scope pocket_kasane_input_scope(bool host_priority);
