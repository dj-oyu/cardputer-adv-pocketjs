#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_http_client.h"

typedef struct {
    esp_http_client_handle_t client;
    bool stream, done, invalid, sse, bitap;
    unsigned reads, bytes, events, delta_bytes, connections;
    unsigned json_objects, json_us, json_max_us, json_live_min;
    unsigned bitap_hits, fallback_count;
    uint32_t delta_hash;
    int64_t first_event, last_event;
    unsigned line_size;
    char line[232];
} ay_chat_probe;

esp_err_t ay_chat_probe_open(ay_chat_probe *probe, uint32_t run, bool bitap);
esp_err_t ay_chat_probe_exchange(ay_chat_probe *probe, const char *phase, unsigned turn, bool cut);
void ay_chat_probe_close(ay_chat_probe *probe);
