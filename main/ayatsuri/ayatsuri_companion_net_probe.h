#ifndef AYATSURI_COMPANION_NET_PROBE_H
#define AYATSURI_COMPANION_NET_PROBE_H

#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"
#ifdef CONFIG_AYATSURI_LCD_GDMA_PRIORITY
#include "hal/lcd_spi.h"
#endif

typedef enum {
    AY_NET_NO_REQUEST,
    AY_NET_RESERVED_RASTER,
    AY_NET_CERT_SHORT_MPI,
    AY_NET_PROJECTION_REUSE,
    AY_NET_RESERVE_WIFI_INIT,
    AY_NET_RESERVE_WIFI_INIT_SMALL_STACK,
    AY_NET_RESERVE_WIFI_INIT_CERT_SCRATCH,
    AY_NET_RESERVE_WIFI_INIT_P384_DETAIL,
    AY_NET_RESERVE_WIFI_INIT_P384_TABLE,
    AY_NET_RESERVE_WIFI_INIT_P384_MODEXP,
    AY_NET_RESERVE_WIFI_INIT_LCD_GDMA,
    AY_NET_RESERVE_WIFI_INIT_RENDER_TAIL,
    AY_NET_RESERVE_WIFI_INIT_CHAT_BITAP,
} ay_net_request;

/* Low 32 bits of the common ESP timer. Each dispatch and its join last
 * far less than one wrap (about 71 minutes). Zero started/queued is valid. */
typedef struct {
    uint32_t queued, notified, joined, started, dispatch;
    uintptr_t core0_at_queue, core0_at_join;
} ay_net_dispatch;

typedef struct {
    uint32_t dispatch, raster, wake;
    ay_net_dispatch peak;
} ay_net_worker_wait;

typedef struct {
    int64_t started, drained, prepare, raster;
    ay_net_worker_wait worker_wait;
    uint32_t band_wait, lcd_wait;
#ifdef CONFIG_AYATSURI_RENDER_TAIL_AB
    uint32_t geometry, submit, final_drain, pose;
    uint32_t deform, wrap, provider;
#endif
#ifdef CONFIG_AYATSURI_LCD_GDMA_PRIORITY
    uint32_t crypto_hits[LCD_GDMA_CRYPTO_COUNT];
#endif
    bool serial;
} ay_net_frame;

extern bool ay_net_measure_frame;
extern ay_net_worker_wait ay_net_frame_worker_wait;

void ayatsuri_companion_net_frame(const ay_net_frame *frame, ay_net_request request);
/* Queried on the UI task until the last frame and result have been reported. */
bool ayatsuri_companion_net_busy(void);
bool ayatsuri_companion_net_reset(void);

#endif
