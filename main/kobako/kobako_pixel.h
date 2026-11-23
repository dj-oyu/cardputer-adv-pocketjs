#pragma once
#include "esp_err.h"
#include "quickjs.h"
#include "ui/kasane/ksn_ports.h"

esp_err_t kobako_pixel_install(JSContext *ctx, JSValueConst kasane);
void kobako_pixel_reset(void);
bool kobako_pixel_pending(void);
void kobako_pixel_present_result(ksn_result result);
