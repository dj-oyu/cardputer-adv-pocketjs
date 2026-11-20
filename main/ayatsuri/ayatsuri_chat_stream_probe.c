#include "ayatsuri_chat_stream_probe.h"
#include "ayatsuri_chat_bitap.h"
#include "ayatsuri_https_hw_profile.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "private/assets/live/chat_probe_endpoint.h"

#define AY_CHAT_STRINGIFY_(value) #value
#define AY_CHAT_STRINGIFY(value) AY_CHAT_STRINGIFY_(value)
#define AY_CHAT_BASE "https://" AY_CHAT_HOST ":" AY_CHAT_STRINGIFY(AY_CHAT_PORT)

static void hash_delta(const char *text, unsigned bytes, uint32_t *hash)
{
    for (unsigned i = 0; i < bytes; ++i)
        *hash = (*hash ^ (unsigned char)text[i]) * UINT32_C(16777619);
}

static void receive_line(ay_chat_probe *probe)
{
    unsigned length = probe->line_size;
    char *line = probe->line;
    if (length && line[length - 1] == '\r') --length;
    if (!length) return;
    if (length == sizeof("data: [DONE]") - 1 && !memcmp(line, "data: [DONE]", length)) {
        probe->invalid |= probe->done || probe->events != 32;
        probe->done = true;
        return;
    }
    static const char prefix[] = "data: ";
    if (probe->done || length < sizeof(prefix) || memcmp(line, prefix, sizeof(prefix) - 1)) {
        probe->invalid = true;
        return;
    }
    line[length] = '\0';
    const int64_t started = esp_timer_get_time();
    char *json = line + sizeof(prefix) - 1;
    const char *content = NULL;
    unsigned bytes = 0;
    cJSON *root = NULL;
    const bool hit = probe->bitap && ay_chat_bitap_delta(json, line + length, probe->events, &content, &bytes);
    if (!hit) {
        root = cJSON_ParseWithLengthOpts(json, (size_t)((line + length + 1) - json), NULL, true);
        const cJSON *sequence = cJSON_GetObjectItemCaseSensitive(root, "n");
        const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
        const cJSON *choice = cJSON_GetArrayItem(choices, 0);
        const cJSON *delta = cJSON_GetObjectItemCaseSensitive(choice, "delta");
        content = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(delta, "content"));
        if (cJSON_IsObject(root) && cJSON_IsNumber(sequence) &&
            sequence->valuedouble == probe->events && cJSON_IsArray(choices) && content && *content)
            bytes = (unsigned)strlen(content);
    }
    if (bytes) hash_delta(content, bytes, &probe->delta_hash);
    const int64_t held = esp_timer_get_time();
    const unsigned free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const int64_t freeing = esp_timer_get_time();
    cJSON_Delete(root);
    if (!bytes) {
        probe->invalid = true;
        return;
    }
    const int64_t now = esp_timer_get_time();
    const unsigned json_us = (unsigned)((held - started) + (now - freeing));
    probe->bitap_hits += hit;
    probe->json_objects += !hit;
    probe->fallback_count += probe->bitap && !hit;
    probe->json_us += json_us;
    if (json_us > probe->json_max_us) probe->json_max_us = json_us;
    if (!probe->json_live_min || free_internal < probe->json_live_min) probe->json_live_min = free_internal;
    probe->delta_bytes += bytes;
    if (!probe->events) probe->first_event = now;
    probe->last_event = now;
    ++probe->events;
}

static esp_err_t chat_http_event(esp_http_client_event_t *event)
{
    ay_chat_probe *probe = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_CONNECTED) ++probe->connections;
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value &&
        !strcasecmp(event->header_key, "Content-Type"))
        probe->sse = !strncmp(event->header_value, "text/event-stream", sizeof("text/event-stream") - 1);
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) return ESP_OK;
    ++probe->reads;
    probe->bytes += (unsigned)event->data_len;
    if (!probe->stream || probe->invalid) return ESP_OK;

    const char *data = event->data;
    for (int i = 0; i < event->data_len; ++i) {
        if (data[i] == '\n') {
            receive_line(probe);
            probe->line_size = 0;
        } else if (probe->line_size < sizeof(probe->line) - 1) {
            probe->line[probe->line_size++] = data[i];
        } else {
            probe->invalid = true;
        }
        if (probe->invalid) break;
    }
    return ESP_OK;
}

esp_err_t ay_chat_probe_open(ay_chat_probe *probe, uint32_t run, bool bitap)
{
    const esp_http_client_config_t config = {
        .url = AY_CHAT_BASE "/ready",
        .cert_pem = ay_chat_root_pem, .common_name = AY_CHAT_PEER,
        .timeout_ms = 4500, .disable_auto_redirect = true,
        .buffer_size = 512, .buffer_size_tx = 512,
        .event_handler = chat_http_event, .user_data = probe,
    };
    *probe = (ay_chat_probe){.bitap = bitap};
    probe->client = esp_http_client_init(&config);
    if (!probe->client) return ESP_ERR_NO_MEM;
    char tag[9];
    snprintf(tag, sizeof tag, "%08" PRIx32, run);
    esp_err_t err = esp_http_client_set_header(probe->client, "X-Ayatsuri-Run", tag);
    if (err == ESP_OK) err = esp_http_client_set_header(probe->client, "Connection", "keep-alive");
    return err;
}

esp_err_t ay_chat_probe_exchange(ay_chat_probe *probe, const char *phase, unsigned turn, bool cut)
{
    static const char body[] = "{\"message\":\"mock turn!\"}";
    esp_http_client_handle_t client = probe->client;
    *probe = (ay_chat_probe){.client = client, .stream = turn != 0, .bitap = probe->bitap,
                             .delta_hash = UINT32_C(2166136261)};
    char url[sizeof(AY_CHAT_BASE "/chat/0000000000/cut")];
    if (turn) snprintf(url, sizeof url, AY_CHAT_BASE "/chat/%u%s", turn, cut ? "/cut" : "");
    else memcpy(url, AY_CHAT_BASE "/ready", sizeof(AY_CHAT_BASE "/ready"));
    esp_err_t err = esp_http_client_set_url(client, url);
    if (err == ESP_OK) err = esp_http_client_set_method(client, turn ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    if (err == ESP_OK && turn) err = esp_http_client_set_header(client, "Content-Type", "application/json");
    /* Each ready GET runs on a newly initialized client with no body. The
     * IDF's null post setter removes Content-Type and errors if it is absent. */
    if (err == ESP_OK && turn) err = esp_http_client_set_post_field(client, body, sizeof(body) - 1);
    if (err != ESP_OK) return err;

    ayatsuri_https_hw_begin();
    const int64_t start = esp_timer_get_time();
    err = esp_http_client_perform(client);
    const int64_t elapsed = esp_timer_get_time() - start;
    ayatsuri_https_hw_end(phase);
    const int status = esp_http_client_get_status_code(client);
    const bool complete = esp_http_client_is_complete_data_received(client);
    const int64_t span = probe->last_event - probe->first_event;
    ESP_LOGI("AY_CHAT", "phase=%s turn=%u cut=%u status=%d err=%s connected=%u complete=%u"
             " reads=%u bytes=%u events=%u delta_bytes=%u done=%u span_us=%" PRId64 " elapsed_us=%" PRId64,
             phase, turn, (unsigned)cut, status, esp_err_to_name(err), probe->connections, (unsigned)complete,
             probe->reads, probe->bytes, probe->events, probe->delta_bytes, (unsigned)probe->done, span, elapsed);
    if (turn) {
        ESP_LOGI("AY_CHAT_JSON", "phase=%s turn=%u cut=%u objects=%u bitap=%u fallback=%u"
                 " total_us=%u max_us=%u live_min=%u delta_hash=%08" PRIx32,
                 phase, turn, (unsigned)cut, probe->json_objects, probe->bitap_hits, probe->fallback_count,
                 probe->json_us, probe->json_max_us, probe->json_live_min, probe->delta_hash);
    }
    if (status != 200 || probe->invalid) return ESP_FAIL;
    if (!turn)
        return err == ESP_OK && complete && probe->connections == 1 && probe->bytes == 6 ? ESP_OK : ESP_FAIL;
    if (!probe->sse || probe->connections || probe->line_size || !probe->delta_bytes) return ESP_FAIL;
    if (cut)
        return !complete && !probe->done && probe->events == 8 && span >= 500000 ? ESP_OK : ESP_FAIL;
    return err == ESP_OK && complete && probe->done && probe->events == 32 &&
           probe->reads >= 8 && span >= 2500000 ? ESP_OK : ESP_FAIL;
}

void ay_chat_probe_close(ay_chat_probe *probe)
{
    if (probe->client) esp_http_client_cleanup(probe->client);
    probe->client = NULL;
}
