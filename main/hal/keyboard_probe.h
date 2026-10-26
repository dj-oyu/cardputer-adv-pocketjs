#pragma once
#include "driver/i2c_master.h"

// Boot only; a successful ACK is required before keyboard register writes.
esp_err_t keyboard_probe_startup(i2c_master_bus_handle_t bus);
