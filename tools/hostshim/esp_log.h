#pragma once
#include <stdio.h>
// The markers tools/test_settings.py and tools/pocket_bridge.py assert on go to
// stdout here too, so a host test can check them without a serial port.
#define ESP_LOGI(tag,fmt,...) printf("I (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag,fmt,...) printf("W (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag,fmt,...) printf("E (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag,fmt,...) ((void)0)
