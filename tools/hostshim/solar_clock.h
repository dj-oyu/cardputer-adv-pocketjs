/* Solar's host tests control the real anchor core without board or ADC IO. */
#include "../../main/system/sys_clock.c"
#include "../../main/system/sys_state.c"
#include "../../main/system/sys_device.h"
static sys_state solar_clock;
static uint64_t solar_mono;
bool sys_device_clock_read(sys_clock_state *out){
    return sys_clock_snapshot(&solar_clock,solar_mono,out);
}
