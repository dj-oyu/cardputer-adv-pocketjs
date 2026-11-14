#pragma once
#include <stdbool.h>
#include <stdint.h>
/* UI-owner API only. Worker owns result until its release publication. */
void edge_device_init(uint64_t boot);
bool edge_device_owned(void);
uint32_t edge_device_instance(void);
bool edge_device_start(void);
void edge_device_bind(const char *id,uint32_t run,uint32_t instance);
bool edge_device_stop(void);
void edge_device_poll(void);
