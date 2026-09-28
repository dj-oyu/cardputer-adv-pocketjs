#ifndef KSN_PROC_COMPILER_DEVICE_PROBE_H
#define KSN_PROC_COMPILER_DEVICE_PROBE_H

#include <stdbool.h>

/* Optional USB-triggered ESP32-S3 diagnostic. Returns false on any mismatch. */
bool ksn_proc_compiler_device_probe_run(void);

#endif
