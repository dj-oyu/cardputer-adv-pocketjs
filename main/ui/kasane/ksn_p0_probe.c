#include "ksn_p0_probe.h"
#ifdef KASANE_P0_PROBE
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#ifdef KASANE_P0_BUS_PROBE
#include <stdatomic.h>
#endif

#define P0_BUCKETS 256u
typedef struct {
    uint32_t bins[P0_BUCKETS];
    uint32_t seen, maximum, over12;
} series;
static series samples[KSN_P0_SAMPLE_COUNT];
#ifdef KASANE_P0_COPY_PROBE
static uint64_t copy_bytes[KSN_P0_COPY_COUNT];
static uint32_t copy_calls[KSN_P0_COPY_COUNT];
typedef struct {const char *text;size_t bytes;} source_text_watch;
static source_text_watch source_text_watches[4];
static uint64_t source_text_core_bytes;
static uint32_t source_text_core_calls,source_text_length_mismatches;
#endif
static uint64_t transfer_bytes,transfer_bands;
static uint32_t transfer_frames;
#ifdef KASANE_P0_BUS_PROBE
/* SD worker updates only the atomic counter; the UI owner alone mutates the
 * frame accumulator. No per-read log or cross-core lock enters the hot path. */
static atomic_uint sd_reads_active;
static atomic_uint sd_read_epoch;
static struct {
    uint32_t swap_us,reap_us,pre_isr_us,post_isr_us,queue_us,sd_reap_calls;
    bool active;
} bus_frame;
static uint32_t bus_sd_reap_calls,bus_missing_isr;

void ksn_p0_bus_sd_begin(void){
    atomic_fetch_add_explicit(&sd_reads_active,1u,memory_order_relaxed);
    atomic_fetch_add_explicit(&sd_read_epoch,1u,memory_order_relaxed);
}
void ksn_p0_bus_sd_end(void){
    atomic_fetch_sub_explicit(&sd_reads_active,1u,memory_order_relaxed);
}
bool ksn_p0_bus_sd_active(void){
    return atomic_load_explicit(&sd_reads_active,memory_order_relaxed)!=0u;
}
uint32_t ksn_p0_bus_sd_epoch(void){
    return atomic_load_explicit(&sd_read_epoch,memory_order_relaxed);
}
void ksn_p0_bus_begin_frame(void){
    bus_frame.swap_us=bus_frame.reap_us=bus_frame.pre_isr_us=0;
    bus_frame.post_isr_us=bus_frame.queue_us=0;
    bus_frame.sd_reap_calls=0;
    bus_frame.active=true;
}
void ksn_p0_bus_phase_sample(ksn_p0_bus_phase phase,uint32_t us,bool sd_overlap){
    if(!bus_frame.active)return;
    if(phase==KSN_P0_BUS_SWAP)bus_frame.swap_us+=us;
    else if(phase==KSN_P0_BUS_REAP){
        bus_frame.reap_us+=us;
        if(sd_overlap)bus_frame.sd_reap_calls++;
    }else if(phase==KSN_P0_BUS_PRE_ISR)bus_frame.pre_isr_us+=us;
    else if(phase==KSN_P0_BUS_POST_ISR)bus_frame.post_isr_us+=us;
    else if(phase==KSN_P0_BUS_QUEUE)bus_frame.queue_us+=us;
}
void ksn_p0_bus_missing_isr(void){bus_missing_isr++;}
void ksn_p0_bus_end_frame(uint32_t send_us){
    if(!bus_frame.active)return;
    bus_frame.active=false;
    ksn_p0_probe_sample(KSN_P0_LCD_SWAP,bus_frame.swap_us);
    ksn_p0_probe_sample(KSN_P0_LCD_REAP,bus_frame.reap_us);
    ksn_p0_probe_sample(KSN_P0_LCD_PRE_ISR,bus_frame.pre_isr_us);
    ksn_p0_probe_sample(KSN_P0_LCD_POST_ISR,bus_frame.post_isr_us);
    ksn_p0_probe_sample(KSN_P0_LCD_QUEUE,bus_frame.queue_us);
    uint32_t phases=bus_frame.swap_us+bus_frame.reap_us+bus_frame.queue_us;
    ksn_p0_probe_sample(KSN_P0_LCD_OTHER,send_us>phases?send_us-phases:0u);
    ksn_p0_probe_sample(bus_frame.sd_reap_calls?KSN_P0_LCD_SEND_SD:
                        KSN_P0_LCD_SEND_IDLE,send_us);
    bus_sd_reap_calls+=bus_frame.sd_reap_calls;
}
#endif
static const char *const sample_names[]={"app_turn","app_render","app_send",
                                         "overlay_work","overlay_draw",
                                         "overlay_send","overlay_compute",
                                         "ui_frame","av_service",
                                         "ui_interval","input_queue"
#ifdef KASANE_P1_OVERLAY_STAGE_PROBE
                                         ,"p1_overlay_guest","p1_overlay_composite",
                                         "p1_music_key","p1_music_output_borrow",
                                         "p1_music_make","p1_music_submit",
                                         "p1_music_fast"
#endif
#ifdef KASANE_P0_BUS_PROBE
                                         ,"lcd_swap","lcd_reap","lcd_pre_isr",
                                         "lcd_post_isr","lcd_queue",
                                         "lcd_other","lcd_send_sd","lcd_send_idle"
#endif
};
// Preserve 128-us resolution for draw/service. Whole-frame and interval
// samples can exceed 32 ms routinely, so give them a 262-ms range without
// charging every other series for a larger histogram.
static const uint16_t sample_bucket_us[]={128,128,128,128,128,128,128,
                                          1024,128,1024,256
#ifdef KASANE_P1_OVERLAY_STAGE_PROBE
                                          ,128,128,128,128,128,128,128
#endif
#ifdef KASANE_P0_BUS_PROBE
                                          ,128,128,128,128,128,128,128,128
#endif
};
_Static_assert(sizeof(sample_names)/sizeof(*sample_names)==KSN_P0_SAMPLE_COUNT,
               "sample probe labels must match the categories");
_Static_assert(sizeof(sample_bucket_us)/sizeof(*sample_bucket_us)==KSN_P0_SAMPLE_COUNT,
               "sample probe bucket widths must match the categories");
#ifdef KASANE_P0_COPY_PROBE
static const char *const copy_names[]={"utf8_request","producer_materialized","adapter_temp",
    "adapter_owned","adapter_slot_commit","schema_ref_commit",
    "music_plan","music_status","music_materialized",
    "music_model_copy","core_payload_write",
    "core_payload_read","core_clone_command","core_clone_text",
    "core_clone_track","core_clone_meta","core_submit_command",
    "core_submit_text","core_render_text","render_decode_view","render_decode_text"};
_Static_assert(sizeof(copy_names)/sizeof(*copy_names)==KSN_P0_COPY_COUNT,
               "copy probe labels must match the categories");
typedef enum {
    P0_GROUP_JS_UTF8_REQUEST,P0_GROUP_PRODUCER,P0_GROUP_ADAPTER_SCHEMA,
    P0_GROUP_CORE_SUBMIT,P0_GROUP_BANK_CLONE,P0_GROUP_RENDER_SCRATCH,
    P0_GROUP_COUNT
} copy_group;
static const char *const group_names[]={"js_utf8_request","producer_snapshot",
    "adapter_schema","core_submit","bank_clone","render_scratch"};
static const uint8_t copy_groups[]={
    [KSN_P0_UTF8_REQUEST]=P0_GROUP_JS_UTF8_REQUEST,
    [KSN_P0_PRODUCER_MATERIALIZED]=P0_GROUP_PRODUCER,
    [KSN_P0_ADAPTER_TEMP]=P0_GROUP_ADAPTER_SCHEMA,
    [KSN_P0_ADAPTER_OWNED]=P0_GROUP_ADAPTER_SCHEMA,
    [KSN_P0_ADAPTER_SLOT_COMMIT]=P0_GROUP_ADAPTER_SCHEMA,
    [KSN_P0_SCHEMA_REF_COMMIT]=P0_GROUP_ADAPTER_SCHEMA,
    [KSN_P0_MUSIC_PLAN]=P0_GROUP_ADAPTER_SCHEMA,
    [KSN_P0_MUSIC_STATUS]=P0_GROUP_ADAPTER_SCHEMA,
    [KSN_P0_MUSIC_MATERIALIZED]=P0_GROUP_ADAPTER_SCHEMA,
    [KSN_P0_MUSIC_MODEL_COPY]=P0_GROUP_ADAPTER_SCHEMA,
    [KSN_P0_CORE_PAYLOAD_WRITE]=P0_GROUP_CORE_SUBMIT,
    [KSN_P0_CORE_PAYLOAD_READ]=P0_GROUP_RENDER_SCRATCH,
    [KSN_P0_CORE_CLONE_COMMAND]=P0_GROUP_BANK_CLONE,
    [KSN_P0_CORE_CLONE_TEXT]=P0_GROUP_BANK_CLONE,
    [KSN_P0_CORE_CLONE_TRACK]=P0_GROUP_BANK_CLONE,
    [KSN_P0_CORE_CLONE_META]=P0_GROUP_BANK_CLONE,
    [KSN_P0_CORE_SUBMIT_COMMAND]=P0_GROUP_CORE_SUBMIT,
    [KSN_P0_CORE_SUBMIT_TEXT]=P0_GROUP_CORE_SUBMIT,
    [KSN_P0_CORE_RENDER_TEXT]=P0_GROUP_RENDER_SCRATCH,
    [KSN_P0_RENDER_DECODE_VIEW]=P0_GROUP_RENDER_SCRATCH,
    [KSN_P0_RENDER_DECODE_TEXT]=P0_GROUP_RENDER_SCRATCH
};
_Static_assert(sizeof(group_names)/sizeof(*group_names)==P0_GROUP_COUNT,
               "copy group names must match");
_Static_assert(sizeof(copy_groups)/sizeof(*copy_groups)==KSN_P0_COPY_COUNT,
               "each copy category needs a group");
#endif

void ksn_p0_probe_reset(void){
    memset(samples,0,sizeof(samples));
#ifdef KASANE_P0_BUS_PROBE
    bus_frame.active=false;bus_sd_reap_calls=bus_missing_isr=0;
#endif
#ifdef KASANE_P0_COPY_PROBE
    memset(copy_bytes,0,sizeof(copy_bytes));
    memset(copy_calls,0,sizeof(copy_calls));
    memset(source_text_watches,0,sizeof(source_text_watches));
    source_text_core_bytes=0;
    source_text_core_calls=source_text_length_mismatches=0;
#endif
    transfer_bytes=transfer_bands=0;transfer_frames=0;
}
#ifdef KASANE_P0_COPY_PROBE
void ksn_p0_probe_copy(ksn_p0_copy_kind kind,size_t bytes){
    if((unsigned)kind>=KSN_P0_COPY_COUNT||!bytes)return;
    copy_bytes[kind]+=bytes;copy_calls[kind]++;
}
bool ksn_p0_probe_watch_source_text(const char *text,size_t bytes){
    if(!text||!bytes)return false;
    for(unsigned i=0;i<sizeof(source_text_watches)/sizeof(*source_text_watches);i++){
        if(source_text_watches[i].text==text){
            source_text_watches[i].bytes=bytes;return true;
        }
    }
    for(unsigned i=0;i<sizeof(source_text_watches)/sizeof(*source_text_watches);i++){
        if(!source_text_watches[i].text){
            source_text_watches[i]=(source_text_watch){text,bytes};return true;
        }
    }
    return false;
}
void ksn_p0_probe_unwatch_source_text(const char *text){
    for(unsigned i=0;i<sizeof(source_text_watches)/sizeof(*source_text_watches);i++)
        if(source_text_watches[i].text==text)source_text_watches[i]=(source_text_watch){0};
}
void ksn_p0_probe_core_source_text(const char *text,size_t bytes){
    if(!text||!bytes)return;
    for(unsigned i=0;i<sizeof(source_text_watches)/sizeof(*source_text_watches);i++){
        if(source_text_watches[i].text!=text)continue;
        source_text_core_calls++;
        source_text_core_bytes+=bytes;
        if(source_text_watches[i].bytes!=bytes)source_text_length_mismatches++;
        return;
    }
}
#endif
void ksn_p0_probe_sample(ksn_p0_sample_kind kind,uint32_t us){
    if((unsigned)kind>=KSN_P0_SAMPLE_COUNT)return;
    series *s=&samples[kind];
    s->seen++;
    if(us>s->maximum)s->maximum=us;
    if(us>12000u)s->over12++;
    /* Exact whole-session counts in metric-specific bins. The last bin is an
     * explicit tail; never present its lower edge as a measured quantile. */
    unsigned bin=us/sample_bucket_us[kind];
    if(bin>=P0_BUCKETS)bin=P0_BUCKETS-1u;
    s->bins[bin]++;
}
void ksn_p0_probe_transfer(uint32_t bytes,uint32_t bands){
    transfer_bytes+=bytes;transfer_bands+=bands;transfer_frames++;
}
static uint32_t rank_upper(const series *s,unsigned percent,uint32_t bucket_us){
    uint32_t at=(uint32_t)(((uint64_t)s->seen*percent+99u)/100u);
    uint32_t count=0;
    for(unsigned i=0;i<P0_BUCKETS;i++){
        count+=s->bins[i];
        if(count>=at)return i==P0_BUCKETS-1u?s->maximum:
            (uint32_t)((i+1u)*bucket_us-1u);
    }
    return s->maximum;
}
void ksn_p0_probe_report(const char *session){
    const char *label=session?session:"unknown";
    for(unsigned k=0;k<KSN_P0_SAMPLE_COUNT;k++){
        series *s=&samples[k];if(!s->seen)continue;
        ESP_LOGI("KSN_P0","S session=%s metric=%s seen=%lu sample=%lu p50=%lu p95=%lu p99=%lu max=%lu over12=%lu method=hist_tailmax bucket_us=%u tail=%lu",
            label,sample_names[k],(unsigned long)s->seen,(unsigned long)s->seen,
            (unsigned long)rank_upper(s,50,sample_bucket_us[k]),
            (unsigned long)rank_upper(s,95,sample_bucket_us[k]),
            (unsigned long)rank_upper(s,99,sample_bucket_us[k]),
            (unsigned long)s->maximum,(unsigned long)s->over12,
            (unsigned)sample_bucket_us[k],(unsigned long)s->bins[P0_BUCKETS-1u]);
    }
#ifdef KASANE_P0_COPY_PROBE
    uint64_t observed_bytes=0;
    uint32_t observed_calls=0;
    uint64_t group_bytes[P0_GROUP_COUNT]={0};
    uint32_t group_calls[P0_GROUP_COUNT]={0};
    for(unsigned k=0;k<KSN_P0_COPY_COUNT;k++){
        /* A ToCString request can borrow ASCII bytes. Keep it visible but
         * never charge its length as an observed memory move. */
        if(k!=KSN_P0_UTF8_REQUEST){
            observed_bytes+=copy_bytes[k];observed_calls+=copy_calls[k];
        }
        group_bytes[copy_groups[k]]+=copy_bytes[k];
        group_calls[copy_groups[k]]+=copy_calls[k];
        if(copy_calls[k])ESP_LOGI("KSN_P0","%s session=%s kind=%s calls=%lu bytes=%llu",
            k==KSN_P0_UTF8_REQUEST?"R":"C",
            label,copy_names[k],(unsigned long)copy_calls[k],
            (unsigned long long)copy_bytes[k]);
    }
    for(unsigned k=0;k<P0_GROUP_COUNT;k++)
        ESP_LOGI("KSN_P0","G session=%s group=%s calls=%lu bytes=%llu coverage=%s",
            label,group_names[k],(unsigned long)group_calls[k],
            (unsigned long long)group_bytes[k],
            k==P0_GROUP_JS_UTF8_REQUEST?"request_not_copy":"partial");
    ESP_LOGI("KSN_P0","C session=%s kind=observed_total calls=%lu bytes=%llu coverage=partial",
        label,(unsigned long)observed_calls,(unsigned long long)observed_bytes);
    ESP_LOGI("KSN_P0","W session=%s source_text_core_calls=%lu bytes=%llu length_mismatches=%lu",
        label,(unsigned long)source_text_core_calls,
        (unsigned long long)source_text_core_bytes,
        (unsigned long)source_text_length_mismatches);
#endif
    if(transfer_frames)ESP_LOGI("KSN_P0","T session=%s frames=%lu lcd_bytes=%llu bands=%llu",
        label,(unsigned long)transfer_frames,(unsigned long long)transfer_bytes,
        (unsigned long long)transfer_bands);
#ifdef KASANE_P0_BUS_PROBE
    ESP_LOGI("KSN_P0","B session=%s sd_reap_calls=%lu sd_active=%u missing_isr=%lu",
        label,(unsigned long)bus_sd_reap_calls,(unsigned)ksn_p0_bus_sd_active(),
        (unsigned long)bus_missing_isr);
#endif
    ESP_LOGI("KSN_P0","M session=%s free=%u min=%u largest=%u stack_free=%u",
        label,(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)uxTaskGetStackHighWaterMark(NULL));
    ksn_p0_probe_reset();
}
#endif
