#ifndef KSN_MESH_Q7_CERTIFIED_H
#define KSN_MESH_Q7_CERTIFIED_H
#include "ksn_mesh_prepared.h"
/* Default-OFF bounded experiment. Q7 coverage/Q16 RGB are approximate; depth
 * and mask retain original arithmetic through certified planes or fallback.
 * Output owns all copied geometry. Caller provides disjoint valid allocations,
 * serializes prepare/render/ACK and holds snapshots alive through render.
 * Float environment must not change during either operation. */
_Static_assert(KSN_PROC_W==240 && KSN_PROC_H==135,"Q7 certificate bounds require 240x135");
typedef struct { double p[2][3],error[2]; bool valid; } ksn_mesh_q7_certificate;
typedef struct { int32_t abc[3][3]; double rgb[2][3][3]; int32_t dx[2][3]; } ksn_mesh_q7_extra;
typedef struct { ksn_prepared_triangle original; ksn_mesh_q7_extra fixed; ksn_mesh_q7_certificate cert; } ksn_mesh_q7_triangle;
typedef struct { uint64_t bands[17]; unsigned count,capacity; bool valid, fallback; ksn_mesh_q7_triangle triangles[]; } ksn_mesh_q7_snapshot;
typedef struct {uint64_t rows,span_pixels,original_pixels,depth_reject,mask_evals,color_fallback,frame_fallback,depth_fast,depth_exact,mask_fast,mask_exact;} ksn_mesh_q7_stats;
size_t ksn_mesh_q7_bytes(const ksn_mesh_prepared *);
bool ksn_mesh_q7_prepare(const ksn_mesh_prepared *,ksn_mesh_q7_snapshot *,size_t);
bool ksn_mesh_q7_render(const ksn_mesh_q7_snapshot *,uint16_t *,unsigned,unsigned,float *,uint8_t *,unsigned,ksn_mesh_q7_stats *);
bool ksn_mesh_q7_span(const ksn_mesh_q7_extra *,int,int *,int *);
#ifdef KSN_Q7_DEPTH_GATE_DIAGNOSTIC
/* Read-only counters. Access/reset only between serialized renderer calls.
 * Separate from the production stats/snapshot ABI; no floating state writes. */
typedef struct {
 uint64_t first_attempts,first_success,inexact_before,inexact_after,inexact_raised,inexact_cleared,inexact_clear;
 uint64_t prepare_attempts,error_reject,inexact_reject,type_reject,coefficient_reject;
 uint64_t radius_reject,scale_reject,endpoint_reject,integer_rows;
 uint64_t integer_pixels,integer_uncertain,integer_carry_reject;
 uint64_t first_fsr_changed,first_fcr_changed,setup_fcr_changed,setup_fsr_changed;
 uint32_t fsr_before_or,fsr_after_or,setup_fsr_before_or,setup_fsr_after_or;bool raw_fsr;
} ksn_mesh_q7_gate_diagnostic;
void ksn_mesh_q7_gate_read(ksn_mesh_q7_gate_diagnostic *out);
void ksn_mesh_q7_gate_reset(void);
#endif
#endif
