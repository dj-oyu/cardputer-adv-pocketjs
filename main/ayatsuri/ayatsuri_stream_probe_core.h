#ifndef AYATSURI_STREAM_PROBE_CORE_H
#define AYATSURI_STREAM_PROBE_CORE_H
#include "ayatsuri_model_stream_candidate.h"
#define AY_STREAM_PROBE_FRAMES 32u
#define AY_STREAM_PROBE_PACKET_CAPACITY 8192u
/* Caller-owned internal CPU memory; no DMA submission is made by this probe. */
typedef struct {
 unsigned char ready_bytes[AY_STREAM_PROBE_PACKET_CAPACITY],pending_bytes[AY_STREAM_PROBE_PACKET_CAPACITY],saved[AY_STREAM_PROBE_PACKET_CAPACITY];
 ayatsuri_skin_result slot[32];float depth[240];uint16_t band[240*8];
} ayatsuri_stream_probe_work;
typedef struct {
 unsigned frames,provider_calls,rollbacks;size_t arena_bytes,workspace_bytes,snapshot_peak;
 uint64_t geometry_us,raster_us,geometry_min_us,geometry_max_us;
 uint32_t final_pixel_hash;bool ready_after_arena_wipe;
} ayatsuri_stream_probe_stats;
typedef uint64_t (*ayatsuri_stream_probe_clock)(void*);
typedef void (*ayatsuri_stream_probe_yield)(void*,unsigned);
size_t ayatsuri_stream_probe_arena_bytes(void);
/* Exclusive CPU-only synthetic diagnostic; clock/yield are trusted platform
 * callbacks, never scripts. Caller provides disjoint aligned arena/work/stats.
 * No allocations, persistence, LCD submission or input model in this routine. */
bool ayatsuri_stream_probe_run(void *arena,size_t arena_bytes,ayatsuri_stream_probe_work*,
 ayatsuri_stream_probe_stats*,ayatsuri_stream_probe_clock,ayatsuri_stream_probe_yield,void*);
#endif
