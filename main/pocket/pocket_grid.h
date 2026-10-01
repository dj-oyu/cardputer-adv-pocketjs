#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "quickjs.h"
#include "ui/kasane/ksn_ports.h"

#define POCKET_GRID_MAX_SLOTS 6u

/* Session-scoped typed grid adapter. An app owns the IR and the image node;
 * the backend owns two output generations until display acknowledgement. */
esp_err_t pocket_grid_install(JSContext *ctx, JSValueConst kasane_namespace);
void pocket_grid_reset(void);
bool pocket_grid_pending(void);
void pocket_grid_present_result(ksn_result result);
void pocket_grid_source_invalidated(uint32_t resource_id);
bool pocket_grid_source_in_use(uint32_t resource_id);
#ifdef KASANE_PROC_DEVICE_PROBE
typedef struct {
    uint64_t source_cycles, kernel_cycles;
    uint32_t source_reads, kernel_spans;
} pocket_grid_resize_profile;
void pocket_grid_resize_profile_read(pocket_grid_resize_profile *out);
#endif
