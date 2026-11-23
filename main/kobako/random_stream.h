#pragma once
#include <stdint.h>

// Non-cryptographic xorshift32-v1. Caller owns each independent stream.
// No allocation, hardware access or shared state in the hot path.
typedef struct { uint32_t state; } kobako_random_t;
static inline void kobako_random_init(kobako_random_t *rng,uint32_t seed) {
    rng->state=seed?seed:UINT32_C(0x6D2B79F5);
}
static inline uint32_t kobako_random_next(kobako_random_t *rng) {
    uint32_t x=rng->state;
    x^=x<<13; x^=x>>17; x^=x<<5;
    return rng->state=x;
}
static inline float kobako_random_float(kobako_random_t *rng) {
    return (float)(kobako_random_next(rng)>>8)*(1.0f/16777216.0f);
}
// Low-frequency seed acquisition. ESP32 uses esp_random; no RF/ADC changes.
// The host implementation is a fixed seed for reproducible renderer tests.
#ifdef ESP_PLATFORM
uint32_t kobako_random_seed(void);
#else
static inline uint32_t kobako_random_seed(void) { return UINT32_C(0x9e3779b9); }
#endif
