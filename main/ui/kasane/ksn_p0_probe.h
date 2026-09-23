#ifndef KSN_P0_PROBE_H
#define KSN_P0_PROBE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Diagnostic only. These categories name actual byte moves at their call
 * sites; UTF-8 materialization is reported separately because QuickJS owns
 * its implementation and may allocate rather than memcpy. */
typedef enum {
    KSN_P0_UTF8_MATERIALIZED,
    KSN_P0_PRODUCER_MATERIALIZED,
    KSN_P0_ADAPTER_TEMP,
    KSN_P0_ADAPTER_OWNED,
    KSN_P0_ADAPTER_SLOT_COMMIT,
    KSN_P0_SCHEMA_REF_COMMIT,
    KSN_P0_MUSIC_PLAN,
    KSN_P0_MUSIC_STATUS,
    KSN_P0_MUSIC_MATERIALIZED,
    KSN_P0_MUSIC_MODEL_COPY,
    KSN_P0_CORE_PAYLOAD_WRITE,
    KSN_P0_CORE_PAYLOAD_READ,
    KSN_P0_CORE_CLONE_COMMAND,
    KSN_P0_CORE_CLONE_TEXT,
    KSN_P0_CORE_CLONE_TRACK,
    KSN_P0_CORE_CLONE_META,
    KSN_P0_CORE_SUBMIT_COMMAND,
    KSN_P0_CORE_SUBMIT_TEXT,
    KSN_P0_CORE_RENDER_TEXT,
    KSN_P0_RENDER_DECODE_VIEW,
    KSN_P0_RENDER_DECODE_TEXT,
    KSN_P0_COPY_COUNT
} ksn_p0_copy_kind;

typedef enum {
    KSN_P0_APP_TURN,
    KSN_P0_APP_RENDER,
    KSN_P0_APP_SEND,
    KSN_P0_OVERLAY_WORK,
    KSN_P0_OVERLAY_DRAW,
    KSN_P0_OVERLAY_SEND,
    KSN_P0_OVERLAY_COMPUTE,
    KSN_P0_UI_FRAME,
    KSN_P0_AV_SERVICE,
    KSN_P0_UI_INTERVAL,
    KSN_P0_INPUT_QUEUE,
#ifdef KASANE_P0_BUS_PROBE
    KSN_P0_LCD_SWAP,
    KSN_P0_LCD_REAP,
    KSN_P0_LCD_QUEUE,
    KSN_P0_LCD_OTHER,
    KSN_P0_LCD_SEND_SD,
    KSN_P0_LCD_SEND_IDLE,
#endif
    KSN_P0_SAMPLE_COUNT
} ksn_p0_sample_kind;

#if defined(KASANE_P0_PROBE) && defined(KASANE_P0_BUS_PROBE)
typedef enum {KSN_P0_BUS_SWAP,KSN_P0_BUS_REAP,KSN_P0_BUS_QUEUE} ksn_p0_bus_phase;
void ksn_p0_bus_begin_frame(void);
void ksn_p0_bus_phase_sample(ksn_p0_bus_phase phase,uint32_t us,bool sd_overlap);
void ksn_p0_bus_end_frame(uint32_t send_us);
void ksn_p0_bus_sd_begin(void);
void ksn_p0_bus_sd_end(void);
bool ksn_p0_bus_sd_active(void);
uint32_t ksn_p0_bus_sd_epoch(void);
#else
static inline void ksn_p0_bus_begin_frame(void) {}
static inline void ksn_p0_bus_phase_sample(int phase,uint32_t us,bool sd_overlap){
    (void)phase;(void)us;(void)sd_overlap;
}
static inline void ksn_p0_bus_end_frame(uint32_t send_us){(void)send_us;}
static inline void ksn_p0_bus_sd_begin(void) {}
static inline void ksn_p0_bus_sd_end(void) {}
static inline bool ksn_p0_bus_sd_active(void){return false;}
static inline uint32_t ksn_p0_bus_sd_epoch(void){return 0;}
#endif

#ifdef KASANE_P0_PROBE
void ksn_p0_probe_reset(void);
#ifdef KASANE_P0_COPY_PROBE
void ksn_p0_probe_copy(ksn_p0_copy_kind kind,size_t bytes);
#else
static inline void ksn_p0_probe_copy(ksn_p0_copy_kind kind,size_t bytes) {
    (void)kind;(void)bytes;
}
#endif
void ksn_p0_probe_sample(ksn_p0_sample_kind kind,uint32_t us);
void ksn_p0_probe_transfer(uint32_t bytes,uint32_t bands);
void ksn_p0_probe_report(const char *session);
#else
static inline void ksn_p0_probe_reset(void) {}
static inline void ksn_p0_probe_copy(ksn_p0_copy_kind kind,size_t bytes) {
    (void)kind;(void)bytes;
}
static inline void ksn_p0_probe_sample(ksn_p0_sample_kind kind,uint32_t us) {
    (void)kind;(void)us;
}
static inline void ksn_p0_probe_transfer(uint32_t bytes,uint32_t bands) {
    (void)bytes;(void)bands;
}
static inline void ksn_p0_probe_report(const char *session) {(void)session;}
#endif

#endif
