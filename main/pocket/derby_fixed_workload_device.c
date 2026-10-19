#include "derby_fixed_workload.h"
#include "esp_timer.h"
#include "usb_console.h"
uint64_t df_now_us(void){return (uint64_t)esp_timer_get_time();}
int df_write(const char *data,size_t size){return usb_console_frame(data,size)==0;}
