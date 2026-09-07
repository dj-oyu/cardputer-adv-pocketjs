#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PET_WIRE_BYTES 48
// The four limits pet_hub.c publishes through capabilities.get(). They live
// here because the structs below are what actually enforces them: an app that
// reads maxLabelChars is reading the size of pet_timer_t::label.
#define PET_MAX_ALERTS  8
#define PET_MAX_TIMERS  4
#define PET_ID_CHARS    16
#define PET_LABEL_CHARS 24
#define PET_HUB_MAGIC 0x50485431u
typedef struct {
    uint32_t stream, sequence, observed, reset[2], notified[2];
    uint64_t tokens;
    uint32_t remainder;
    int32_t used[2];
    bool baseline;
} pet_usage_t;
typedef struct {
    uint32_t magic, selected, food[12], wake_day;
    int32_t wake_minute, utc_offset;
    pet_usage_t usage[2];
} pet_hub_saved_t;
typedef struct {
    char id[PET_ID_CHARS+1], label[PET_LABEL_CHARS+1];
    uint64_t due;
} pet_timer_t;
typedef struct {
    pet_hub_saved_t saved;
    pet_timer_t timers[PET_MAX_TIMERS];
    char alerts[PET_MAX_ALERTS][PET_LABEL_CHARS+1];
    unsigned read, count;
} pet_hub_t;
uint32_t pet_crc(const uint8_t *p, unsigned n);
void pet_hub_defaults(pet_hub_t *h);
bool pet_hub_packet(pet_hub_t *h, const uint8_t data[PET_WIRE_BYTES]);
bool pet_hub_timer(pet_hub_t *h, const char *id, const char *label, uint64_t due);
bool pet_hub_notify(pet_hub_t *h, const char *label);
bool pet_hub_take(pet_hub_t *h, char label[PET_LABEL_CHARS+1]);
bool pet_hub_tick(pet_hub_t *h, uint64_t ms, uint32_t utc);
