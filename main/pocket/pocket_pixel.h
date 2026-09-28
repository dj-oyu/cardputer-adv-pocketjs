#pragma once
#include "esp_err.h"
#include "quickjs.h"
#include "ui/kasane/ksn_ports.h"

esp_err_t pocket_pixel_install(JSContext *ctx, JSValueConst kasane);
void pocket_pixel_reset(void);
bool pocket_pixel_pending(void);
void pocket_pixel_present_result(ksn_result result);
