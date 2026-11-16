"""Compile production VM owner admission with synthetic task/ISR identities."""
import argparse, pathlib, subprocess, tempfile
p=argparse.ArgumentParser();p.add_argument('--root',required=True);a=p.parse_args()
r=pathlib.Path(a.root).resolve()
with tempfile.TemporaryDirectory() as td:
 d=pathlib.Path(td);(d/'freertos').mkdir()
 (d/'freertos/FreeRTOS.h').write_text('''#pragma once
#include <stdint.h>
#include <stddef.h>
typedef void *TaskHandle_t; typedef unsigned TickType_t; typedef int BaseType_t;
#define pdFALSE 0
#define pdTRUE 1
extern TaskHandle_t current; extern int isr;
static inline int xPortInIsrContext(void){return isr;}
#define portYIELD_FROM_ISR() ((void)0)
''')
 (d/'freertos/task.h').write_text('''#pragma once
#include "FreeRTOS.h"
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void){return current;}
static inline void xTaskNotifyGive(TaskHandle_t t){(void)t;}
static inline void vTaskNotifyGiveFromISR(TaskHandle_t t,BaseType_t*w){(void)t;*w=0;}
static inline uint32_t ulTaskNotifyTake(int c,TickType_t t){(void)c;(void)t;return 0;}
''')
 (d/'test.c').write_text('''#include "vm_wake.h"
#include <assert.h>
TaskHandle_t current; int isr;
int main(void){int a,b;current=&a;assert(!vm_wake_is_owner());vm_wake_bind();assert(vm_wake_is_owner());current=&b;assert(!vm_wake_is_owner());current=&a;isr=1;assert(!vm_wake_is_owner());isr=0;assert(vm_wake_is_owner());return 0;}
''')
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(d),'-I'+str(r/'main/vm'),str(r/'main/vm/vm_wake.c'),str(d/'test.c'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
print('production owner predicate: unbound / owner / foreign-task / ISR PASS (synthetic RTOS identities)')
