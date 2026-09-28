#ifndef KSN_PROC_LIMITS_DEVICE_PROBE_H
#define KSN_PROC_LIMITS_DEVICE_PROBE_H

#include "esp_err.h"
#include "quickjs.h"

/* KASANE_PROC_LIMITS_PROBE only (default OFF). USB '{' at HOME runs the
 * native measurements; USB '}' starts proc_limits_probe.js, which reaches
 * heap/stack/clock readings through the __lim global installed here. See
 * docs/kasane/procedural-limits-device.md. */
void ksn_proc_limits_device_probe_run(void);
esp_err_t ksn_proc_limits_js_install(JSContext *ctx, void *user_data);

#endif
