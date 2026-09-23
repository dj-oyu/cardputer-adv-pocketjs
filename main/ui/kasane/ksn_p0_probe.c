#include "ksn_p0_probe.h"
#ifdef KASANE_P0_PROBE
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

#define P0_BUCKETS 256u
typedef struct {
    uint32_t bins[P0_BUCKETS];
    uint32_t seen, maximum, over12;
} series;
static series samples[KSN_P0_SAMPLE_COUNT];
static uint64_t copy_bytes[KSN_P0_COPY_COUNT];
static uint32_t copy_calls[KSN_P0_COPY_COUNT];
static uint64_t transfer_bytes,transfer_bands;
static uint32_t transfer_frames;
static const char *const sample_names[]={"app_turn","app_render","app_send",
                                         "overlay_work","overlay_draw",
                                         "overlay_send","overlay_compute",
                                         "ui_frame","av_service",
                                         "ui_interval","input_queue"};
// Preserve 128-us resolution for draw/service. Whole-frame and interval
// samples can exceed 32 ms routinely, so give them a 262-ms range without
// charging every other series for a larger histogram.
static const uint16_t sample_bucket_us[]={128,128,128,128,128,128,128,
                                          1024,128,1024,256};
_Static_assert(sizeof(sample_names)/sizeof(*sample_names)==KSN_P0_SAMPLE_COUNT,
               "sample probe labels must match the categories");
_Static_assert(sizeof(sample_bucket_us)/sizeof(*sample_bucket_us)==KSN_P0_SAMPLE_COUNT,
               "sample probe bucket widths must match the categories");
static const char *const copy_names[]={"utf8_materialized","producer_materialized","adapter_temp",
    "adapter_owned","music_plan","music_status","music_materialized",
    "music_model_copy","core_payload_write",
    "core_payload_read","core_clone_command","core_clone_text",
    "core_clone_track","core_submit_text","core_render_text","render_decode_text"};
_Static_assert(sizeof(copy_names)/sizeof(*copy_names)==KSN_P0_COPY_COUNT,
               "copy probe labels must match the categories");

void ksn_p0_probe_reset(void){
    memset(samples,0,sizeof(samples));memset(copy_bytes,0,sizeof(copy_bytes));
    memset(copy_calls,0,sizeof(copy_calls));
    transfer_bytes=transfer_bands=0;transfer_frames=0;
}
void ksn_p0_probe_copy(ksn_p0_copy_kind kind,size_t bytes){
    if((unsigned)kind>=KSN_P0_COPY_COUNT||!bytes)return;
    copy_bytes[kind]+=bytes;copy_calls[kind]++;
}
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
    uint64_t observed_bytes=0;
    uint32_t observed_calls=0;
    for(unsigned k=0;k<KSN_P0_COPY_COUNT;k++){
        observed_bytes+=copy_bytes[k];observed_calls+=copy_calls[k];
        if(copy_calls[k])ESP_LOGI("KSN_P0","C session=%s kind=%s calls=%lu bytes=%llu",
            label,copy_names[k],(unsigned long)copy_calls[k],
            (unsigned long long)copy_bytes[k]);
    }
    ESP_LOGI("KSN_P0","C session=%s kind=observed_total calls=%lu bytes=%llu coverage=partial",
        label,(unsigned long)observed_calls,(unsigned long long)observed_bytes);
    if(transfer_frames)ESP_LOGI("KSN_P0","T session=%s frames=%lu lcd_bytes=%llu bands=%llu",
        label,(unsigned long)transfer_frames,(unsigned long long)transfer_bytes,
        (unsigned long long)transfer_bands);
    ESP_LOGI("KSN_P0","M session=%s free=%u min=%u largest=%u stack_free=%u",
        label,(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)uxTaskGetStackHighWaterMark(NULL));
    ksn_p0_probe_reset();
}
#endif
