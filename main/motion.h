#pragma once
#include "driver/i2c_master.h"
void motion_init(i2c_master_bus_handle_t bus);
void motion_poll(void);
void motion_get(int *x, int *y);
void motion_recenter(void);
