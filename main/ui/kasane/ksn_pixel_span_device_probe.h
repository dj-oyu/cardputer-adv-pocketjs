#ifndef KSN_PIXEL_SPAN_DEVICE_PROBE_H
#define KSN_PIXEL_SPAN_DEVICE_PROBE_H

#include <stdbool.h>

/* Optional Kasane diagnostic. It renders two 120-frame image/overlay scenes
 * through the real compositor and LCD, then exercises retry and repair. */
bool ksn_pixel_span_device_probe_run(void);

#endif
