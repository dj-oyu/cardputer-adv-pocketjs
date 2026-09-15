#ifndef SYS_DEVICE_H
#define SYS_DEVICE_H
#include "sys_state.h"
#include "sys_notify.h"
/* Firmware owner adapter. State survives guest attach/detach. */
sys_state *sys_device_state(void);
sys_notify *sys_device_notifications(void);
void sys_device_step(void);
bool sys_device_clock_read(sys_clock_state *out);
#endif
