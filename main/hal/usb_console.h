#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// After driver install, before tasks: stdout/stderr and bridge share one writer.
esp_err_t usb_console_init(void);
// A bridge frame is queued whole or refused; never replay an operation.
esp_err_t usb_console_frame(const void *data, size_t size);
// ESP_LOG lines go out as one write each (default). Off restores the
// piecewise path; only the frame_bd log A/B turns it off.
void usb_console_log_whole(bool on);
