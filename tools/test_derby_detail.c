#include "derby_detail.h"
#include "derby_measure.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static uint64_t clock_us;
static int task=1,isr;
uint64_t dm_now_us(void){return clock_us;}
void *xTaskGetCurrentTaskHandle(void){return (void *)(uintptr_t)task;}
int xPortInIsrContext(void){return isr;}
static void early(void){DD_SCOPE(DD_VM_RUN);clock_us+=7;return;}
static void overflow(unsigned n){DD_SCOPE(DD_VM_RUN);if(n)overflow(n-1);clock_us++;}
int main(void){
    dd_snapshot s;
    dd_start();dd_js(DD_JS_FRAME);clock_us+=3;
    {DD_SCOPE(DD_PROC_BUILD);clock_us+=5;early();clock_us+=2;}
    task=2;{DD_SCOPE(DD_VM_RUN);DD_ADD(DD_VM_ZERO_BYTES,999);dd_js(-1);}task=1;
    isr=1;{DD_SCOPE(DD_VM_RUN);DD_ADD(DD_VM_ZERO_BYTES,999);dd_js(-1);}isr=0;
    DD_ADD(DD_VM_ZERO_BYTES,996);clock_us+=4;
    assert(!dd_finish(&s));dd_js(-1);assert(dd_finish(&s));
    assert(!s.faults&&s.pairs==3&&s.event[DD_VM_ZERO_BYTES]==996);
    assert(s.stage[DD_JS_FRAME].inclusive_us==21&&s.stage[DD_JS_FRAME].self_us==7);
    assert(s.stage[DD_PROC_BUILD].inclusive_us==14&&s.stage[DD_PROC_BUILD].self_us==7);
    assert(s.stage[DD_VM_RUN].inclusive_us==7&&s.stage[DD_VM_RUN].calls==1);
    dd_start();dd_js(DD_JS_FRAME);clock_us+=10;dd_js(DD_SORT);clock_us+=5;
    dd_suspend();assert(!dd_finish(&s));
    {DD_SCOPE(DD_RENDER);clock_us+=20;}clock_us+=1000; /* explicit pace */
    dd_resume();clock_us+=7;dd_js(-1);clock_us+=3;dd_js(-1);
    assert(dd_finish(&s)&&!s.faults);
    assert(s.stage[DD_JS_FRAME].inclusive_us==25&&s.stage[DD_JS_FRAME].self_us==13);
    assert(s.stage[DD_SORT].inclusive_us==12&&s.stage[DD_SORT].self_us==12);
    assert(s.stage[DD_RENDER].inclusive_us==20&&s.stage[DD_RENDER].self_us==20);
    dd_start();dd_js(DD_JS_FRAME);dd_suspend();dd_cancel();assert(!dd_finish(&s));
    dd_start();overflow(25);assert(dd_finish(&s)&&s.faults==2);
    dd_start();dd_js(-1);assert(dd_finish(&s)&&s.faults==1);
    dd_start();{DD_SCOPE(DD_VM_RUN);clock_us=0;}assert(dd_finish(&s)&&s.faults==1);
    dd_start();DD_ADD(DD_VM_ZERO_BYTES,UINT64_MAX);DD_ADD(DD_VM_ZERO_BYTES,1);
    assert(dd_finish(&s)&&s.faults==1);
    dd_start();dd_scope a=dd_enter(DD_JS_FRAME),b=dd_enter(DD_VM_RUN);
    dd_leave(&a);dd_leave(&b);assert(!dd_finish(&s));
    puts("PASS detail exclusive/inclusive, return cleanup, foreign task/ISR ignored, active read refused, overflow/clock/order faults");
}
