#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Owner-task only. Sampled counters, never allocator peaks or LCD proof. */
typedef struct {
    uint32_t seed, race, step, phase, camera, tier;
    int32_t u_ppm; /* -1 outside gate/race: photo uses its frozen crossing. */
} derby_telemetry_context;
#ifdef POCKET_DERBY_TELEMETRY
void derby_telemetry_reset(void);
/* Invalidate before a new guest frame; park/failure cannot label old-bank work. */
void derby_telemetry_begin_frame(void);
void derby_telemetry_set(derby_telemetry_context context);
void derby_telemetry_eval(size_t used, size_t cap);
void derby_telemetry_io_us(uint32_t elapsed);
void derby_telemetry_present(int64_t now_us, size_t used, size_t cap);
/* Implementation supplied by the platform; no retry on refusal. */
bool derby_telemetry_write(const char *line, size_t length);
#endif
