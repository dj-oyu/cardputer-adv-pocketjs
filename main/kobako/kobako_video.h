#pragma once
#include "esp_err.h"
#include "quickjs.h"
#include "ui/kasane/ksn_ports.h"

esp_err_t kobako_video_install(JSContext *ctx,JSValueConst kasane);
void kobako_video_reset(void);
bool kobako_video_pending(void);
void kobako_video_present_result(ksn_result result);
