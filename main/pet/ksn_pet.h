#ifndef KSN_PET_H
#define KSN_PET_H
#include "ksn_ports.h"
/* Validate once before registering. The borrowed PPT2 bytes remain immutable
 * and alive until this owner's resources are reset. No global pet selection. */
ksn_result ksn_pet_image(const uint8_t *data,size_t bytes,ksn_image_port *out);
ksn_result ksn_pet_builtin_image(ksn_image_port *out);
/* Decoded-row cache for the span reader above, keyed by the borrowed image
 * bytes, the variant and the frame (default on). On, a row is decoded once and
 * reused by every span of that row; the source row set of a 64x64 pet is
 * 64 x 128 B in internal SRAM. Off restores the pre-cache path, which decodes
 * the whole 64-pixel row on every read_span call. The cached pixels are the
 * decoders' own output, so both arms return the same bytes. */
extern bool g_ksn_pet_row_cache;
#ifdef KSN_PET_ROW_STATS
/* Host-only fetch/decode counters; the firmware build does not define
 * KSN_PET_ROW_STATS and carries neither counter. */
extern unsigned long long g_ksn_pet_fetches,g_ksn_pet_decodes;
#endif
#endif
