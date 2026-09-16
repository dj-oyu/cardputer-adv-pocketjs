// MEASUREMENT HARNESS -- worktree only, never committed, never part of pocket.ble.
//
// Answers the half of the BLE question a link map cannot: what the NimBLE host
// and the BLE controller take from the HEAP when the stack is actually started.
// Static .bss is measurable without a board; this is not.
//
// BLE_PROBE_RUN=0 (default): the stack is linked but never started, so the map
//   reports its true static DIRAM and the device reports the free heap a build
//   that merely CONTAINS BLE would leave.
// BLE_PROBE_RUN=1: the stack is started at boot and the free heap is logged
//   either side of it, so the delta IS the runtime cost.
#include "sdkconfig.h"
#include <stdbool.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

#if CONFIG_BT_ENABLED
#include "esp_bt.h"
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"

#ifndef BLE_PROBE_RUN
#define BLE_PROBE_RUN 0
#endif

volatile bool ble_probe_never = false;

static void report(const char *stage) {
    ESP_LOGI("blemem", "BLEMEM %s free=%u largest=%u", stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

void ble_probe(void) {
    report("linked");
#if BLE_PROBE_RUN
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_init(&cfg) != ESP_OK) { ESP_LOGE("blemem","ctrl_init failed"); return; }
    report("ctrl_init");
    if (esp_bt_controller_enable(ESP_BT_MODE_BLE) != ESP_OK) { ESP_LOGE("blemem","ctrl_enable failed"); return; }
    report("ctrl_enable");
    nimble_port_init();
    report("host_init");
#else
    // Never true. Keeps the linker from discarding what the map must account for.
    if (ble_probe_never) {
        esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        esp_bt_controller_init(&cfg);
        esp_bt_controller_enable(ESP_BT_MODE_BLE);
        nimble_port_init();
        ble_gap_disc(0, 1000, NULL, NULL, NULL);
        nimble_port_run();
    }
#endif
}
#else
void ble_probe(void) {
    ESP_LOGI("blemem", "BLEMEM linked free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}
#endif
