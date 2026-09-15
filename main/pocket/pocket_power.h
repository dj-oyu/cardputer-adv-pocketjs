#ifndef POCKET_POWER_H
#define POCKET_POWER_H
#include "esp_err.h"
#include "quickjs.h"
esp_err_t pocket_power_install(JSContext *);
void pocket_power_pump(void);
void pocket_power_reset(void);
#endif
