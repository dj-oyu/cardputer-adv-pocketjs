#pragma once
#include <stdbool.h>
#include "driver/i2c_master.h"
void sound_init(i2c_master_bus_handle_t bus);
void sound_set_enabled(bool enabled);
void sound_play(int kind);
