#ifndef KSN_PROC_POINTS_PIE_H
#define KSN_PROC_POINTS_PIE_H

#include "ksn_proc_points.h"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

/* Keep dispatch and implementation on the same gate. ESP-IDF supplies the
 * target through sdkconfig.h, not necessarily a compiler -D option. */
#if defined(__XTENSA__) && defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_IDF_TARGET_ESP32S3
#define KSN_PROC_POINTS_HAS_PIE 1
#else
#define KSN_PROC_POINTS_HAS_PIE 0
#endif

/* Same contract as ksn_proc_points_affine_scalar. Full eight-point blocks use
 * ESP32-S3 PIE; the final 1..7 points use the scalar reference. */
void ksn_proc_points_affine_pie(KsnProcPointDst dst,
                                KsnProcPointSrc src, size_t n,
                                const KsnProcAffineQ14 *coeff);

#endif
