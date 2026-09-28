#pragma once
#include "esp_err.h"
#include "quickjs.h"
#include "ui/kasane/ksn_ports.h"

esp_err_t pocket_video_install(JSContext *ctx,JSValueConst kasane);
void pocket_video_reset(void);
bool pocket_video_pending(void);
void pocket_video_present_result(ksn_result result);
