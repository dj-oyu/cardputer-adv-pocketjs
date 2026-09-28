#ifndef KSN_GRID_SCAN_COST_DEVICE_PROBE_H
#define KSN_GRID_SCAN_COST_DEVICE_PROBE_H

#include <stdbool.h>

/* Diagnostic image only. Times the existing row-wise scan PIE lowering. */
bool ksn_grid_scan_cost_device_probe_run(void);

#endif
