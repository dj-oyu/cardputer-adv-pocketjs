#ifndef AYATSURI_MODEL_STREAM_CANDIDATE_H
#define AYATSURI_MODEL_STREAM_CANDIDATE_H
#include "ayatsuri_model_provider.h"
#include "../ui/kasane/ksn_mesh_depth.h"
/* Explicit native-only prototype. Compile ayatsuri_model_stream_candidate.c
 * separately; not a default model API, wire flag, JS entry or runtime admission.
 * Model/blob are borrowed immutable until dispose. Instance must start zeroed.
 * Do not access core or pass it to ordinary APIs. No retained mesh view exists. */
typedef struct {ayatsuri_model_instance core;uint32_t tag;} ayatsuri_stream_candidate;
/* Open immutable group/mesh provider-only capability; out unchanged on failure. */
ayatsuri_model_status ayatsuri_stream_candidate_open(const void*,size_t,ayatsuri_model*);
size_t ayatsuri_stream_candidate_bytes(const ayatsuri_model *model);
ayatsuri_model_status ayatsuri_stream_candidate_init(const ayatsuri_model*,void*,size_t,const void*,uint32_t,ayatsuri_stream_candidate*);
ayatsuri_model_status ayatsuri_stream_candidate_targets(ayatsuri_stream_candidate*,const void*,uint32_t,const ayatsuri_model_targets*);
ayatsuri_model_status ayatsuri_stream_candidate_dispose(ayatsuri_stream_candidate*,const void*,uint32_t);
/* Dedicated pending snapshot only: disjoint from READY/pinned/DMA-owned storage.
 * Success only completes pending; it does not publish or pin. Failure may leave
 * private pending/candidate work partial, and MUST preserve the separate ready
 * owner. Caller supplies immutable source inputs and a 32-result provider slot.
 * Do not rebuild/reuse pending while a reader or display submission owns it.
 * Exclusive synchronous trusted-native owner only; not ISR/thread safe.
 * Provider callback restrictions are identical to ayatsuri_model_provider.h. */
ayatsuri_model_status ayatsuri_stream_candidate_frame(ayatsuri_stream_candidate*,const void*,uint32_t,
 const float camera[9],const float light[6],ayatsuri_model_geometry_provider,void*,
 ayatsuri_skin_result slot[32],ksn_mesh_snapshot *pending);
#endif
