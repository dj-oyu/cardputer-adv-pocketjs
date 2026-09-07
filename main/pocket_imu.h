#pragma once
#include "esp_err.h"
#include "quickjs.h"

// pocket.sensors.imu — section 8 of docs/common-api.md, on top of motion.c.
//
// Everything here runs on the JS owner task. pocket_imu_pump() is what drives
// the watch subscriptions and must be called once per frame from that same
// task, before the guest's frame function; without it latest() still works and
// watches simply never fire.

esp_err_t pocket_imu_install(JSContext *ctx, void *user_data);

// Delivers to the open watches whose period has elapsed. Cheap and safe to call
// when nothing is subscribed: it returns after one load and one branch.
void pocket_imu_pump(void);

// Closes every watch. Call from the JS task while the guest is still alive --
// app_stop() before it destroys the guest -- so the callbacks are released and
// the gyroscope goes back off.
void pocket_imu_reset(void);
