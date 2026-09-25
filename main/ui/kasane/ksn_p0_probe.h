#ifndef KSN_P0_PROBE_H
#define KSN_P0_PROBE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Diagnostic only. Most categories name byte moves at their call sites.
 * UTF8_REQUEST is only the length passed to JS_ToCStringLen: QuickJS may
 * return a borrowed ASCII pointer, so it is not a copy or allocation count. */
typedef enum {
    KSN_P0_UTF8_REQUEST,
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
#ifdef KASANE_P1_OVERLAY_STAGE_PROBE
    KSN_P1_OVERLAY_GUEST,
    KSN_P1_OVERLAY_COMPOSITE,
    KSN_P1_MUSIC_KEY,
    KSN_P1_MUSIC_OUTPUT_BORROW,
    KSN_P1_MUSIC_MAKE,
    KSN_P1_MUSIC_SUBMIT,
    KSN_P1_MUSIC_FAST,
#endif
#ifdef KASANE_P0_BUS_PROBE
    KSN_P0_LCD_SWAP,
    KSN_P0_LCD_REAP,
    KSN_P0_LCD_PRE_ISR,
    KSN_P0_LCD_POST_ISR,
    KSN_P0_LCD_QUEUE,
    KSN_P0_LCD_OTHER,
    KSN_P0_LCD_SEND_SD,
    KSN_P0_LCD_SEND_IDLE,
#endif
    KSN_P0_SAMPLE_COUNT
} ksn_p0_sample_kind;

#if defined(KASANE_P0_PROBE) && defined(KASANE_P0_BUS_PROBE)
typedef enum {
    KSN_P0_BUS_SWAP,KSN_P0_BUS_REAP,KSN_P0_BUS_PRE_ISR,
    KSN_P0_BUS_POST_ISR,KSN_P0_BUS_QUEUE
} ksn_p0_bus_phase;
void ksn_p0_bus_begin_frame(void);
void ksn_p0_bus_phase_sample(ksn_p0_bus_phase phase,uint32_t us,bool sd_overlap);
void ksn_p0_bus_end_frame(uint32_t send_us);
void ksn_p0_bus_missing_isr(void);
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
static inline void ksn_p0_bus_missing_isr(void) {}
static inline void ksn_p0_bus_sd_begin(void) {}
static inline void ksn_p0_bus_sd_end(void) {}
static inline bool ksn_p0_bus_sd_active(void){return false;}
static inline uint32_t ksn_p0_bus_sd_epoch(void){return 0;}
#endif

#ifdef KASANE_P0_PROBE
void ksn_p0_probe_reset(void);
#ifdef KASANE_P0_COPY_PROBE
void ksn_p0_probe_copy(ksn_p0_copy_kind kind,size_t bytes);
/* Diagnostic pointer identity only: a producer may register its immutable
 * published text buffers. Core submit reports when it copies directly from
 * one of those addresses. This is a subset of CORE_SUBMIT_TEXT, not an extra
 * copy category. Registration and core submit run on the owner task. */
bool ksn_p0_probe_watch_source_text(const char *text,size_t bytes);
void ksn_p0_probe_unwatch_source_text(const char *text);
void ksn_p0_probe_core_source_text(const char *text,size_t bytes);
#else
static inline void ksn_p0_probe_copy(ksn_p0_copy_kind kind,size_t bytes) {
    (void)kind;(void)bytes;
}
static inline bool ksn_p0_probe_watch_source_text(const char *text,size_t bytes) {
    (void)text;(void)bytes;return true;
}
static inline void ksn_p0_probe_unwatch_source_text(const char *text) {(void)text;}
static inline void ksn_p0_probe_core_source_text(const char *text,size_t bytes) {
    (void)text;(void)bytes;
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
static inline bool ksn_p0_probe_watch_source_text(const char *text,size_t bytes) {
    (void)text;(void)bytes;return true;
}
static inline void ksn_p0_probe_unwatch_source_text(const char *text) {(void)text;}
static inline void ksn_p0_probe_core_source_text(const char *text,size_t bytes) {
    (void)text;(void)bytes;
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
