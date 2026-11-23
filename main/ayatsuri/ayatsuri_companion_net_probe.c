#include "ayatsuri_companion_net_probe.h"
#include "ayatsuri_https_hw_profile.h"

#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kobako/net_service.h"
#include "kobako/wifi_time.h"
#include "sdkconfig.h"
#ifdef CONFIG_AYATSURI_CHAT_STREAM_PROBE
#include "ayatsuri_chat_stream_probe.h"
#include "esp_random.h"
#define AY_NET_TLS_PEER "local-chat-stream"
static bool chat_use_bitap;
#else
#define AY_NET_TLS_PEER "example.com"
#endif
#ifdef CONFIG_AYATSURI_P384_PROFILE
#include "ayatsuri_p384_profile.h"
bool ay_p384_vector_run(bool detailed);
static bool p384_detail_enabled;
#endif
#ifdef CONFIG_AYATSURI_CERT_INV_SCRATCH
#include "ayatsuri_cert_inv_scratch.h"
#endif
#ifdef CONFIG_AYATSURI_RENDER_TAIL_AB
void ay_live_model_render_tail_opt(bool enabled);
uint32_t ay_live_model_render_tail_hits(void);
#endif

enum phase_id {
    BASELINE, WIFI_CONNECT, CLOCK_SYNC, HTTPS_CONNECT, HTTPS_RECONNECT,
    HTTPS_KEEP_FIRST, HTTPS_KEEP_REUSE,
    WIFI_DISCONNECT, WIFI_RECONNECT, HTTPS_AFTER_WIFI, LINK_IDLE, RELEASE,
#ifdef CONFIG_AYATSURI_CHAT_STREAM_PROBE
    CHAT_FIRST_TLS, CHAT_STEADY, CHAT_CUT, CHAT_RECOVERY_TLS,
    CHAT_RECOVERED, CHAT_WIFI_TLS, CHAT_AFTER_WIFI,
#endif
#ifdef CONFIG_AYATSURI_LCD_GDMA_PRIORITY
    GDMA_AES, GDMA_SHA,
#endif
#ifdef CONFIG_AYATSURI_SHA_DMA_PROBE
    SHA_STOCK_A, SHA_IRQ_A, SHA_IRQ_B, SHA_STOCK_B,
#endif
    PHASE_COUNT,
};

static const char *const phase_names[] = {
    "baseline", "wifi_connect", "clock_sync", "https_connect", "https_reconnect",
    "https_keep_first", "https_keep_reuse",
    "wifi_disconnect", "wifi_reconnect", "https_after_wifi", "link_idle", "release",
#ifdef CONFIG_AYATSURI_CHAT_STREAM_PROBE
    "chat_first_tls", "chat_steady", "chat_cut", "chat_recovery_tls",
    "chat_recovered", "chat_wifi_tls", "chat_after_wifi",
#endif
#ifdef CONFIG_AYATSURI_LCD_GDMA_PRIORITY
    "gdma_aes", "gdma_sha",
#endif
#ifdef CONFIG_AYATSURI_SHA_DMA_PROBE
    "sha_stock_a", "sha_irq_a", "sha_irq_b", "sha_stock_b",
#endif
};
static const char tag[] = "AY_NET_PROBE";
static const char holder[] = "ayatsuri_probe";
#ifndef CONFIG_AYATSURI_CHAT_STREAM_PROBE
static const char url[] = "https://example.com/";
#endif
static const uint32_t heap_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

#ifdef CONFIG_AYATSURI_LCD_GDMA_PRIORITY
typedef struct {
    uint32_t transfers, frames, wait_max, interval_max;
    uint64_t wait_sum;
} gdma_overlap;

esp_err_t ayatsuri_lcd_gdma_load(bool aes, const atomic_bool *cancel);
#endif

typedef struct {
    int64_t begin, end, interval_sum, longest;
    unsigned local_frames;
    ay_net_frame worst;
    ay_net_dispatch peak_dispatch;
    uint32_t max_band_wait, max_lcd_wait;
    uint64_t band_wait_sum, lcd_wait_sum;
    unsigned intervals, over_55ms, min_free, min_largest, samples;
    unsigned sdk_low_in, sdk_low_out;
#ifdef CONFIG_AYATSURI_CHAT_STREAM_PROBE
    unsigned over_50ms;
#endif
#ifdef CONFIG_AYATSURI_RENDER_TAIL_AB
    uint32_t prepare_sum, raster_sum, deform_sum, wrap_sum, provider_sum;
#endif
#ifdef CONFIG_AYATSURI_LCD_GDMA_PRIORITY
    gdma_overlap crypto[LCD_GDMA_CRYPTO_COUNT];
#endif
} phase_stats;

static phase_stats phases[PHASE_COUNT];
static phase_stats total;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static atomic_bool running, cancel;
static int active = -1;
static esp_timer_handle_t sampler;
static int64_t last_drain, ended_at;
static esp_err_t outcome;
static enum phase_id final_phase;
static bool launched, reported, cert_short_mpi;
static unsigned worker_stack_bytes, worker_stack_hwm;
bool ay_net_measure_frame;
ay_net_worker_wait ay_net_frame_worker_wait;

static void sample_heap(void *arg)
{
    (void)arg;
    const int64_t at = esp_timer_get_time();
    multi_heap_info_t info;
    heap_caps_get_info(&info, heap_caps);
    portENTER_CRITICAL(&lock);
    if (active >= 0 && at >= phases[active].begin) {
        phase_stats *s = &phases[active];
        if (info.total_free_bytes < s->min_free) s->min_free = info.total_free_bytes;
        if (info.largest_free_block < s->min_largest) s->min_largest = info.largest_free_block;
        ++s->samples;
    }
    portEXIT_CRITICAL(&lock);
}

/* The worker owns phase boundaries. The UI counts full DMA intervals that
 * intersect each phase, including the interval spanning either boundary. */
static void begin_phase(enum phase_id id)
{
    const unsigned sdk_low = heap_caps_get_minimum_free_size(heap_caps);
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&lock);
    if (active >= 0) {
        phases[active].end = now;
        phases[active].sdk_low_out = sdk_low;
    }
    active = id;
    phases[id] = (phase_stats){.begin=now, .min_free=UINT_MAX, .min_largest=UINT_MAX,
                              .sdk_low_in=sdk_low};
    portEXIT_CRITICAL(&lock);
    final_phase = id;
    sample_heap(NULL);
}

static bool pause_ms(unsigned ms)
{
    const int64_t until = esp_timer_get_time() + ms * INT64_C(1000);
    while (!atomic_load(&cancel) && esp_timer_get_time() < until) vTaskDelay(pdMS_TO_TICKS(20));
    return !atomic_load(&cancel);
}

static esp_err_t connect_wifi(bool *held)
{
    esp_err_t err = net_service_acquire(holder);
    if (err != ESP_OK) return err;
    *held = true;
    const int64_t deadline = esp_timer_get_time() + INT64_C(14000000);
    while (!atomic_load(&cancel) && esp_timer_get_time() < deadline) {
        net_service_state_t state = net_service_state();
        if (state == NET_SERVICE_UP) {
            sample_heap(NULL);
            ESP_LOGI(tag, "WIFI phase=%s connected=1", phase_names[final_phase]);
            return ESP_OK;
        }
        if (state == NET_SERVICE_FAILED) {
            wifi_time_status_t status = wifi_time_status();
            ESP_LOGW(tag, "WIFI_FAILURE stage=%s reason=%d", wifi_time_stage_name(status.stage), status.reason);
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return atomic_load(&cancel) ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
}

static esp_err_t release_wifi(bool *held)
{
    if (*held) {
        esp_err_t err = net_service_release(holder);
        if (err != ESP_OK) return err;
        *held = false;
    }
    const int64_t deadline = esp_timer_get_time() + INT64_C(3000000);
    while (wifi_time_busy() && esp_timer_get_time() < deadline) vTaskDelay(pdMS_TO_TICKS(20));
    return wifi_time_busy() ? ESP_ERR_TIMEOUT : ESP_OK;
}

#ifndef CONFIG_AYATSURI_CHAT_STREAM_PROBE
typedef struct {
    unsigned response_bytes;
    int64_t connected_at, first_header_at;
} http_observation;

static esp_err_t observe_http_event(esp_http_client_event_t *event)
{
    http_observation *observation = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_CONNECTED)
        observation->connected_at = esp_timer_get_time();
    if (event->event_id == HTTP_EVENT_ON_HEADER && !observation->first_header_at)
        observation->first_header_at = esp_timer_get_time();
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0)
        observation->response_bytes += (unsigned)event->data_len;
    return ESP_OK;
}

#if configGENERATE_RUN_TIME_STATS
static uint32_t runtime_ticks(TaskHandle_t task)
{
    TaskStatus_t status;
    vTaskGetInfo(task, &status, pdFALSE, eInvalid);
    return status.ulRunTimeCounter;
}
#endif

static esp_err_t https_exchange(esp_http_client_handle_t client, http_observation *observation)
{
    *observation = (http_observation){0};
    const char *phase = phase_names[final_phase];
#if configGENERATE_RUN_TIME_STATS
    const TaskHandle_t self = xTaskGetCurrentTaskHandle();
    vTaskDelay(1);
    const uint32_t worker_before = runtime_ticks(self);
    const uint32_t idle0_before = ulTaskGetIdleRunTimeCounterForCore(0);
    const uint32_t idle1_before = ulTaskGetIdleRunTimeCounterForCore(1);
#endif
    ayatsuri_https_hw_begin();
    const int64_t started = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(client);
    const int64_t end = esp_timer_get_time();
#if configGENERATE_RUN_TIME_STATS
    vTaskDelay(1);  /* Charge the final running slice to the worker. */
    const uint32_t worker_ticks = runtime_ticks(self) - worker_before;
    const uint32_t idle0_ticks = ulTaskGetIdleRunTimeCounterForCore(0) - idle0_before;
    const uint32_t idle1_ticks = ulTaskGetIdleRunTimeCounterForCore(1) - idle1_before;
    ESP_LOGI(tag, "HTTPS_CPU phase=%s worker_ticks=%" PRIu32 " idle0_ticks=%" PRIu32
             " idle1_ticks=%" PRIu32 " stack_hwm=%u", phase, worker_ticks, idle0_ticks, idle1_ticks,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
#endif
    ayatsuri_https_hw_end(phase);

    const int status = esp_http_client_get_status_code(client);
    if (err == ESP_OK && (status != 200 || !esp_http_client_is_complete_data_received(client)))
        err = ESP_FAIL;
    sample_heap(NULL);
    ESP_LOGI(tag, "HTTPS phase=%s status=%d error=%s bytes=%u elapsed_us=%" PRId64
             " connected_us=%" PRId64 " first_header_us=%" PRId64,
             phase, status, esp_err_to_name(err), observation->response_bytes, end - started,
             observation->connected_at ? observation->connected_at - started : -1,
             observation->first_header_at ? observation->first_header_at - started : -1);
    return err;
}

static esp_err_t https_request(bool keep)
{
    http_observation observation = {0};
    const esp_http_client_config_t config = {
        .url=url, .method=HTTP_METHOD_GET, .timeout_ms=4500,
        .crt_bundle_attach=esp_crt_bundle_attach, .disable_auto_redirect=true,
        .buffer_size=512, .buffer_size_tx=512, .event_handler=observe_http_event,
        .user_data=&observation,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_http_client_set_header(client, "Connection", keep ? "keep-alive" : "close");
    if (err == ESP_OK) err = https_exchange(client, &observation);
    if (err == ESP_OK && keep && !atomic_load(&cancel)) {
        begin_phase(HTTPS_KEEP_REUSE);
        err = https_exchange(client, &observation);
    }
    esp_http_client_cleanup(client);
    return err;
}
#else
static esp_err_t chat_segment(uint32_t run, enum phase_id dial, enum phase_id receive,
                              unsigned first, unsigned turns, bool interrupt)
{
    ay_chat_probe probe;
    begin_phase(dial);
    esp_err_t err = ay_chat_probe_open(&probe, run, chat_use_bitap);
    if (err == ESP_OK) err = ay_chat_probe_exchange(&probe, phase_names[dial], 0, false);
    if (err == ESP_OK) {
        begin_phase(receive);
        for (unsigned turn = first; err == ESP_OK && turn < first + turns; ++turn) {
            if (atomic_load(&cancel)) {
                err = ESP_ERR_INVALID_STATE;
                break;
            }
            err = ay_chat_probe_exchange(&probe, phase_names[receive], turn, false);
            sample_heap(NULL);
        }
    }
    if (err == ESP_OK && interrupt && !atomic_load(&cancel)) {
        begin_phase(CHAT_CUT);
        err = ay_chat_probe_exchange(&probe, phase_names[CHAT_CUT], first + turns, true);
        sample_heap(NULL);
    }
    ay_chat_probe_close(&probe);
    return atomic_load(&cancel) && err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
}

static esp_err_t measure_chat(bool *held)
{
    const uint32_t run = esp_random();
    ESP_LOGI(tag, "CHAT_BEGIN run=%08" PRIx32 " parser=%s", run, chat_use_bitap ? "bitap" : "cjson");
    esp_err_t err = chat_segment(run, CHAT_FIRST_TLS, CHAT_STEADY, 1, 2, true);
    if (err != ESP_OK) return err;
    err = chat_segment(run, CHAT_RECOVERY_TLS, CHAT_RECOVERED, 3, 1, false);
    if (err != ESP_OK) return err;
    begin_phase(WIFI_DISCONNECT);
    err = release_wifi(held);
    if (err != ESP_OK) return err;
    if (!pause_ms(450)) return ESP_ERR_INVALID_STATE;
    begin_phase(WIFI_RECONNECT);
    err = connect_wifi(held);
    if (err != ESP_OK) return err;
    return chat_segment(run, CHAT_WIFI_TLS, CHAT_AFTER_WIFI, 4, 2, false);
}
#endif

static esp_err_t measure_network(bool *held)
{
    const bool kat_ok = ayatsuri_https_hw_short_mul_selftest();
    ESP_LOGI(tag, "CERT_MPI_KAT status=%s", kat_ok ? "PASS" : "FAIL");
    if (!kat_ok) return ESP_FAIL;
#ifdef CONFIG_AYATSURI_P384_MODEXP
    const bool modexp_kat_ok = ay_p384_modexp_selftest();
    ESP_LOGI(tag, "P384_MODEXP_KAT status=%s", modexp_kat_ok ? "PASS" : "FAIL");
    if (!modexp_kat_ok) return ESP_FAIL;
#endif
#ifdef CONFIG_AYATSURI_P384_PROFILE
    if (!ay_p384_vector_run(p384_detail_enabled)) return ESP_FAIL;
#endif
#ifdef CONFIG_AYATSURI_CERT_INV_SCRATCH
    const bool scratch_kat_ok = ay_cert_inv_scratch_selftest();
    ESP_LOGI(tag, "CERT_INV_KAT status=%s", scratch_kat_ok ? "PASS" : "FAIL");
    if (!scratch_kat_ok) return ESP_FAIL;
#endif
    ayatsuri_https_hw_set_short_mul(cert_short_mpi);
    if (!pause_ms(1500)) return ESP_ERR_INVALID_STATE;
#ifdef CONFIG_AYATSURI_LCD_GDMA_PRIORITY
    for (enum phase_id phase = GDMA_AES; phase <= GDMA_SHA; ++phase) {
        begin_phase(phase);
        ayatsuri_https_hw_begin();
        const esp_err_t result = ayatsuri_lcd_gdma_load(phase == GDMA_AES, &cancel);
        ayatsuri_https_hw_end(phase_names[phase]);
        if (result != ESP_OK) return result;
    }
#endif
#ifdef CONFIG_AYATSURI_SHA_DMA_PROBE
    extern esp_err_t ayatsuri_sha_dma_window(bool irq, const atomic_bool *cancel);
    for (enum phase_id phase = SHA_STOCK_A; phase <= SHA_STOCK_B; ++phase) {
        begin_phase(phase);
        esp_err_t result = ayatsuri_sha_dma_window(phase == SHA_IRQ_A || phase == SHA_IRQ_B, &cancel);
        if (result != ESP_OK) return result;
    }
#endif
    if (!wifi_time_has_credentials()) {
        ESP_LOGW(tag, "WIFI_FAILURE no_saved_profile=1");
        return ESP_ERR_NOT_FOUND;
    }
    begin_phase(WIFI_CONNECT);
    esp_err_t err = connect_wifi(held);
    if (err != ESP_OK) return err;

#ifdef CONFIG_MBEDTLS_HAVE_TIME_DATE
    if (time(NULL) < INT64_C(1735689600)) {
        begin_phase(CLOCK_SYNC);
        err = wifi_time_link_sntp_start();
        if (err != ESP_OK) return err;
        for (unsigned i = 0; i < 60 && !atomic_load(&cancel); ++i) {
            err = wifi_time_link_sntp_wait(100);
            if (err != ESP_ERR_TIMEOUT) break;
        }
        wifi_time_link_sntp_stop();
        if (err != ESP_OK) return err;
    }
#endif

#ifdef CONFIG_AYATSURI_CHAT_STREAM_PROBE
    err = measure_chat(held);
#else
    begin_phase(HTTPS_CONNECT);
    err = https_request(false);
    if (err != ESP_OK) return err;
    if (atomic_load(&cancel)) return ESP_ERR_INVALID_STATE;
    begin_phase(HTTPS_RECONNECT);
    err = https_request(false);
    if (err != ESP_OK) return err;
    if (atomic_load(&cancel)) return ESP_ERR_INVALID_STATE;

    begin_phase(HTTPS_KEEP_FIRST);
    err = https_request(true);
    if (err != ESP_OK) return err;
    if (atomic_load(&cancel)) return ESP_ERR_INVALID_STATE;

    begin_phase(WIFI_DISCONNECT);
    err = release_wifi(held);
    if (err != ESP_OK) return err;
    if (!pause_ms(450)) return ESP_ERR_INVALID_STATE;
    begin_phase(WIFI_RECONNECT);
    err = connect_wifi(held);
    if (err != ESP_OK) return err;
    begin_phase(HTTPS_AFTER_WIFI);
    err = https_request(false);
#endif
    if (err != ESP_OK) return err;
    begin_phase(LINK_IDLE);
    return pause_ms(1500) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static void net_worker(void *arg)
{
    (void)arg;
    bool held = false;
    outcome = measure_network(&held);
    if (held) {
        const enum phase_id last = final_phase;
        begin_phase(RELEASE);
        const esp_err_t released = release_wifi(&held);
        if (outcome == ESP_OK) outcome = released;
        else final_phase = last;
    }
    worker_stack_hwm = (unsigned)uxTaskGetStackHighWaterMark(NULL);
    sample_heap(NULL);
    esp_timer_stop(sampler);
    const unsigned sdk_low = heap_caps_get_minimum_free_size(heap_caps);
    ended_at = esp_timer_get_time();
    portENTER_CRITICAL(&lock);
    phases[active].end = ended_at;
    phases[active].sdk_low_out = sdk_low;
    active = -1;
    portEXIT_CRITICAL(&lock);
    atomic_store(&running, false);
    vTaskDelete(NULL);
}

static void start(ay_net_request request)
{
    const esp_timer_create_args_t timer = {.callback=sample_heap, .name="ay_net_heap"};
    launched = true;
    cert_short_mpi = request == AY_NET_CERT_SHORT_MPI;
    const bool small_stack = request == AY_NET_RESERVE_WIFI_INIT_SMALL_STACK;
    const bool fix = request == AY_NET_RESERVE_WIFI_INIT_CERT_SCRATCH;
    const bool detail = request == AY_NET_RESERVE_WIFI_INIT_P384_DETAIL;
    const bool gdma_high = request == AY_NET_RESERVE_WIFI_INIT_LCD_GDMA;
    const bool render_tail = request == AY_NET_RESERVE_WIFI_INIT_RENDER_TAIL;
    const bool bitap = request == AY_NET_RESERVE_WIFI_INIT_CHAT_BITAP;
    const bool modexp = gdma_high || render_tail || bitap || request == AY_NET_RESERVE_WIFI_INIT_P384_MODEXP;
    const bool table = modexp || request == AY_NET_RESERVE_WIFI_INIT_P384_TABLE;
    const bool reserve_init = small_stack || fix || detail || table || request == AY_NET_RESERVE_WIFI_INIT;
    worker_stack_bytes = small_stack ? 5376 : 6144;
    worker_stack_hwm = 0;
    ESP_LOGI(tag, "BEGIN heap_sample_us=10000 tls_peer=" AY_NET_TLS_PEER " mode=reserved mpi_cert=%s"
             " projection=%s init=%s stack_bytes=%u cause_fix=%s p384_detail=%u p384_table=%u p384_modexp=%u gdma_high=%u render_tail=%u",
             cert_short_mpi ? "short_sw" : "stock",
             request == AY_NET_PROJECTION_REUSE ? "reuse" : "stock",
             reserve_init ? "reserved" : "stock", worker_stack_bytes, fix ? "candidate" : "stock",
             (unsigned)detail, (unsigned)table, (unsigned)modexp, (unsigned)gdma_high,
             (unsigned)render_tail);
    outcome = esp_timer_create(&timer, &sampler);
    if (outcome != ESP_OK) return;
    outcome = esp_timer_start_periodic(sampler, 10000);
    if (outcome != ESP_OK) return;
#ifdef CONFIG_AYATSURI_LCD_GDMA_PRIORITY
    outcome = lcd_spi_gdma_probe_start(gdma_high);
    if (outcome != ESP_OK) {
        esp_timer_stop(sampler);
        ESP_LOGE(tag, "LCD_GDMA setup failed: %s", esp_err_to_name(outcome));
        return;
    }
#endif
    wifi_time_probe_trace_begin();
    wifi_time_probe_reserve_init(reserve_init);
#ifdef CONFIG_AYATSURI_P384_PROFILE
    p384_detail_enabled = detail;
    ay_p384_set_detail(detail);
#endif
#ifdef CONFIG_AYATSURI_P384_G_TABLE
    ay_p384_set_table(table);
#endif
#ifdef CONFIG_AYATSURI_P384_MODEXP
    ay_p384_set_modexp(modexp);
#endif
#ifdef CONFIG_AYATSURI_CHAT_STREAM_PROBE
    chat_use_bitap = bitap;
#endif
#ifdef CONFIG_AYATSURI_RENDER_TAIL_AB
    ay_live_model_render_tail_opt(render_tail);
#endif
#ifdef CONFIG_AYATSURI_CERT_INV_SCRATCH
    ayatsuri_https_hw_use_cert_inv_scratch(fix);
#endif
    begin_phase(BASELINE);
    atomic_store(&running, true);
    if (xTaskCreatePinnedToCore(net_worker, "ay_net_probe", worker_stack_bytes, NULL, 4, NULL, 0) != pdPASS) {
        atomic_store(&running, false);
        esp_timer_stop(sampler);
#ifdef CONFIG_AYATSURI_LCD_GDMA_PRIORITY
        lcd_spi_gdma_probe_finish();
#endif
        outcome = ESP_ERR_NO_MEM;
    }
}

void ayatsuri_companion_net_frame(const ay_net_frame *frame, ay_net_request request)
{
    const int64_t drained = frame->drained;
    if (!launched) {
        if (request == AY_NET_NO_REQUEST) return;
        last_drain = drained;
        start(request);
        return;
    }
    if (reported) return;
    const int64_t interval = drained - last_drain;
    bool overlaps = false;
    portENTER_CRITICAL(&lock);
    for (unsigned i = 0; i < PHASE_COUNT; ++i) {
        phase_stats *s = &phases[i];
        if (!s->begin || drained <= s->begin || (s->end && last_drain >= s->end)) continue;
        if (i != BASELINE) overlaps = true;
        ++s->intervals;
        if (frame->serial) ++s->local_frames;
        s->interval_sum += interval;
        s->band_wait_sum += frame->band_wait;
        s->lcd_wait_sum += frame->lcd_wait;
#ifdef CONFIG_AYATSURI_RENDER_TAIL_AB
        /* Each diagnostic app session is bounded to 90 s. Its phase sums
         * fit in 32-bit microseconds. */
        s->prepare_sum += (uint32_t)frame->prepare;
        s->raster_sum += (uint32_t)frame->raster;
        s->deform_sum += frame->deform;
        s->wrap_sum += frame->wrap;
        s->provider_sum += frame->provider;
#endif
        if (frame->band_wait > s->max_band_wait) s->max_band_wait = frame->band_wait;
        if (frame->lcd_wait > s->max_lcd_wait) s->max_lcd_wait = frame->lcd_wait;
#ifdef CONFIG_AYATSURI_LCD_GDMA_PRIORITY
        /* Assign hits to the phase containing the rendering work. */
        if (frame->started >= s->begin && (!s->end || drained <= s->end)) {
            for (unsigned engine = 0; engine < LCD_GDMA_CRYPTO_COUNT; ++engine) {
                if (!frame->crypto_hits[engine]) continue;
                gdma_overlap *g = &s->crypto[engine];
                g->transfers += frame->crypto_hits[engine];
                ++g->frames;
                g->wait_sum += frame->lcd_wait;
                if (frame->lcd_wait > g->wait_max) g->wait_max = frame->lcd_wait;
                if (interval > g->interval_max) g->interval_max = (uint32_t)interval;
            }
        }
#endif
        if (frame->worker_wait.peak.dispatch > s->peak_dispatch.dispatch)
            s->peak_dispatch = frame->worker_wait.peak;
        if (interval > s->longest) {
            s->longest = interval;
            s->worst = *frame;
        }
        if (interval > 55000) ++s->over_55ms;
#ifdef CONFIG_AYATSURI_CHAT_STREAM_PROBE
        if (interval > 50000) ++s->over_50ms;
#endif
    }
    portEXIT_CRITICAL(&lock);
    if (overlaps) {
        ++total.intervals;
        total.interval_sum += interval;
        if (interval > total.longest) total.longest = interval;
        if (interval > 55000) ++total.over_55ms;
#ifdef CONFIG_AYATSURI_CHAT_STREAM_PROBE
        if (interval > 50000) ++total.over_50ms;
#endif
    }
    last_drain = drained;
    if (atomic_load(&running) || drained < ended_at) return;

    reported = true;
#ifdef CONFIG_AYATSURI_LCD_GDMA_PRIORITY
    lcd_spi_gdma_probe_finish();
#endif
    const unsigned sdk_low = heap_caps_get_minimum_free_size(heap_caps);
    wifi_time_probe_reserve_init(false);
    wifi_time_probe_trace_report();
#ifdef CONFIG_AYATSURI_RENDER_TAIL_AB
    ESP_LOGI(tag, "RENDER_TAIL basis_reuses=%" PRIu32, ay_live_model_render_tail_hits());
#endif
    for (unsigned i = 0; i < PHASE_COUNT; ++i) {
        const phase_stats *s = &phases[i];
        if (!s->begin) continue;
        ESP_LOGI(tag, "PHASE name=%s elapsed_us=%" PRId64 " intervals=%u interval_sum_us=%" PRId64
                 " begin_us=%" PRId64 " end_us=%" PRId64
                 " longest_us=%" PRId64 " over55=%u min_free=%u min_largest=%u samples=%u local_frames=%u",
                 phase_names[i], s->end ? s->end - s->begin : 0, s->intervals, s->interval_sum,
                 s->begin, s->end,
                 s->longest, s->over_55ms, s->min_free, s->min_largest, s->samples, s->local_frames);
#ifdef CONFIG_AYATSURI_CHAT_STREAM_PROBE
        ESP_LOGI(tag, "CHAT_DEADLINE name=%s over50=%u", phase_names[i], s->over_50ms);
#endif
        ESP_LOGI(tag, "GAP name=%s start_us=%" PRId64 " drain_us=%" PRId64 " prepare_us=%" PRId64
                 " raster_us=%" PRId64 " band_wait_us=%" PRIu32 " serial=%u"
                 " worker_dispatch_us=%" PRIu32 " worker_raster_us=%" PRIu32
                 " worker_wake_us=%" PRIu32 " lcd_wait_us=%" PRIu32,
                 phase_names[i], s->worst.started, s->worst.drained, s->worst.prepare,
                 s->worst.raster, s->worst.band_wait, (unsigned)s->worst.serial,
                 s->worst.worker_wait.dispatch, s->worst.worker_wait.raster,
                 s->worst.worker_wait.wake, s->worst.lcd_wait);
#ifdef CONFIG_AYATSURI_RENDER_TAIL_AB
        ESP_LOGI(tag, "GAP_RENDER name=%s pose=%" PRIu32 " idle_us=%" PRId64
                 " geometry_us=%" PRIu32 " submit_us=%" PRIu32 " final_drain_us=%" PRIu32,
                 phase_names[i], s->worst.pose, s->longest - (s->worst.drained - s->worst.started),
                 s->worst.geometry, s->worst.submit, s->worst.final_drain);
        ESP_LOGI(tag, "GAP_MODEL name=%s deform_us=%" PRIu32 " wrap_us=%" PRIu32 " provider_us=%" PRIu32,
                 phase_names[i], s->worst.deform, s->worst.wrap, s->worst.provider);
        ESP_LOGI(tag, "RENDER_COST name=%s prepare_sum_us=%" PRIu32 " raster_sum_us=%" PRIu32
                 " deform_sum_us=%" PRIu32 " wrap_sum_us=%" PRIu32 " provider_sum_us=%" PRIu32,
                 phase_names[i], s->prepare_sum, s->raster_sum, s->deform_sum,
                 s->wrap_sum, s->provider_sum);
#endif
        ESP_LOGI(tag, "WAITS name=%s band_sum_us=%" PRIu64 " lcd_sum_us=%" PRIu64
                 " band_max_us=%" PRIu32 " lcd_max_us=%" PRIu32,
                 phase_names[i], s->band_wait_sum, s->lcd_wait_sum, s->max_band_wait, s->max_lcd_wait);
#ifdef CONFIG_AYATSURI_LCD_GDMA_PRIORITY
        for (unsigned engine = 0; engine < LCD_GDMA_CRYPTO_COUNT; ++engine) {
            const gdma_overlap *g = &s->crypto[engine];
            ESP_LOGI(tag, "DMA_OVERLAP name=%s engine=%s starts=%" PRIu32 " frames=%" PRIu32
                     " lcd_sum_us=%" PRIu64 " lcd_max_us=%" PRIu32 " interval_max_us=%" PRIu32,
                     phase_names[i], engine == LCD_GDMA_AES ? "aes" : "sha",
                     g->transfers, g->frames, g->wait_sum, g->wait_max, g->interval_max);
        }
#endif
        ESP_LOGI(tag, "HEAP_LOW name=%s sdk_in=%u sdk_out=%u sample_min=%u min_largest=%u",
                 phase_names[i], s->sdk_low_in, s->sdk_low_out, s->min_free, s->min_largest);
        const ay_net_dispatch peak = s->peak_dispatch;
        ESP_LOGI(tag, "DISPATCH name=%s queued_us=%" PRIu32 " joined_us=%" PRIu32
                 " notify_return_us=%" PRIu32 " started_us=%" PRIu32 " dispatch_us=%" PRIu32
                 " core0_at_queue=%" PRIuPTR " core0_at_join=%" PRIuPTR,
                 phase_names[i], peak.queued, peak.joined, peak.notified, peak.started, peak.dispatch,
                 peak.core0_at_queue, peak.core0_at_join);
    }
    ESP_LOGI(tag, "HEAP_REGIONS_BEGIN");
    heap_caps_print_heap_info(heap_caps);
    ESP_LOGI(tag, "HEAP_REGIONS_END");
    ESP_LOGI(tag, "WORKER_STACK bytes=%u min_unused=%u", worker_stack_bytes, worker_stack_hwm);
#ifdef CONFIG_AYATSURI_CHAT_STREAM_PROBE
    ESP_LOGI(tag, "CHAT_DEADLINE name=whole over50=%u", total.over_50ms);
#endif
    ESP_LOGI(tag, "RESULT error=%s phase=%s min_free=%u intervals=%u interval_sum_us=%" PRId64
             " longest_us=%" PRId64 " over55=%u", esp_err_to_name(outcome), phase_names[final_phase],
             sdk_low, total.intervals,
             total.interval_sum, total.longest, total.over_55ms);
}

bool ayatsuri_companion_net_busy(void)
{
    return launched && !reported;
}

bool ayatsuri_companion_net_reset(void)
{
    if (atomic_load(&running)) {
        atomic_store(&cancel, true);
        return false;
    }
    wifi_time_probe_reserve_init(false);
#ifdef CONFIG_AYATSURI_RENDER_TAIL_AB
    ay_live_model_render_tail_opt(false);
#endif
#ifdef CONFIG_AYATSURI_LCD_GDMA_PRIORITY
    lcd_spi_gdma_probe_finish();
#endif
    if (sampler) {
        esp_timer_stop(sampler);
        if (esp_timer_delete(sampler) != ESP_OK) return false;
        sampler = NULL;
    }
    portENTER_CRITICAL(&lock);
    memset(phases, 0, sizeof phases);
    total = (phase_stats){0};
    active = -1;
    portEXIT_CRITICAL(&lock);
    atomic_store(&cancel, false);
    ended_at = 0;
    launched = reported = false;
    return true;
}
