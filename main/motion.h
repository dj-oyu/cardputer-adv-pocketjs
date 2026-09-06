#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"

// One IMU reading in the frame docs/common-api.md section 8 publishes: x to the
// right, y up, z out of the screen toward the viewer, with the display upright.
// Acceleration is the accelerometer's own output including gravity, in m/s^2;
// angular rate is right-handed, in rad/s. roll and pitch are the resting
// inclination the spec defines, atan2(ay,az) and atan2(-ax,hypot(ay,az)), in
// radians, and mean nothing while the device is being moved.
typedef struct {
    int64_t time_us;      // esp_timer_get_time() when the sample was read
    uint32_t sequence;    // 1 for the first sample, then one per sample taken
    uint32_t dropped;     // estimated samples missed since motion_init
    float accel_x, accel_y, accel_z;
    float gyro_x, gyro_y, gyro_z;
    float roll, pitch;
    bool gyro_valid;      // false while the gyroscope is off or still starting
} motion_sample_t;

void motion_init(i2c_master_bus_handle_t bus);
void motion_poll(void);
void motion_get(int *x, int *y);
void motion_recenter(void);

// True if a BMI270 answered at init. False means every other call here is inert,
// and the capability is unsupported on this unit rather than merely unavailable.
bool motion_present(void);
// The rate motion_poll reads at, in Hz. A caller asking for more than this
// cannot be served.
unsigned motion_rate_hz(void);
// Copies the newest sample. No I2C: this only reads what motion_poll left
// behind. False if the IMU is absent or has not produced a sample yet.
bool motion_latest(motion_sample_t *out);
// Asks for the gyroscope to be switched on or off. The change is applied by the
// next motion_poll rather than here, so this never touches the bus from the
// caller's thread; watch gyro_valid in the samples to see it take effect. The
// gyroscope is off after motion_init because it draws several times the
// accelerometer's current (datasheet figure, not measured here).
void motion_request_gyro(bool on);
