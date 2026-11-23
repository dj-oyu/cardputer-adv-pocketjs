#ifndef KOBAKO_POWER_H
#define KOBAKO_POWER_H
#include "esp_err.h"
#include "quickjs.h"
esp_err_t kobako_power_install(JSContext *);
void kobako_power_pump(void);
void kobako_power_reset(void);
#endif
