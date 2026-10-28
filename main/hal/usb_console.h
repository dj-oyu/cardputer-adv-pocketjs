#pragma once
#include <stddef.h>
#include "esp_err.h"

// After driver install, before tasks: stdout/stderr and bridge share one writer.
esp_err_t usb_console_init(void);
// A bridge frame is queued whole or refused; never replay an operation.
esp_err_t usb_console_frame(const void *data, size_t size);
