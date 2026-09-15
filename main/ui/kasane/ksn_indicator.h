#ifndef KSN_INDICATOR_H
#define KSN_INDICATOR_H
#include "ksn_view.h"
/* Host-owned microphone indicator, appended after notifications in the same
 * SYSTEM transaction. Eight opaque rectangles, no text or retained pointers.
 * lit is the number of illuminated cells (0..6). Inactive emits nothing.
 * The caller owns microphone state, quantization, deadlines and transaction. */
typedef struct { bool active; uint8_t lit; bool clipping; } ksn_recording_indicator;
ksn_result ksn_recording_emit(ksn_view *,ksn_tx,ksn_recording_indicator);
#endif
