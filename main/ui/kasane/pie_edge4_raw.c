/* Same reviewed ten PIE operations, shared interface; target execution unverified. */
#include "pie_edge4.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if defined(__XTENSA__) && !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "PIE edge4 raw requires ESP32-S3, not another Xtensa target"
#endif
_Alignas(16) static const int32_t limit[4]={-190,-190,-190,-190};
#if defined(__XTENSA__)
__attribute__((noinline))
void pie_edge4_reject_raw(const pie_edge4_lanes *in,pie_edge4_mask *out){
 const int32_t *p=&in->v[0][0],*t=limit;uint32_t *o=out->v;
 __asm__ volatile(
  "ee.vld.128.ip q0, %[p], 16\n"
  "ee.vld.128.ip q1, %[p], 16\n"
  "ee.vld.128.ip q2, %[p], 16\n"
  "ee.vld.128.ip q3, %[t], 0\n"
  "ee.vcmp.lt.s32 q5, q0, q3\n"
  "ee.vcmp.lt.s32 q4, q1, q3\n"
  "ee.orq q5, q5, q4\n"
  "ee.vcmp.lt.s32 q4, q2, q3\n"
  "ee.orq q5, q5, q4\n"
  "ee.vst.128.ip q5, %[o], 0\n"
  :[p]"+&a"(p),[t]"+&a"(t),[o]"+&a"(o)::"memory");
}
#else
void pie_edge4_reject_raw(const pie_edge4_lanes *in,pie_edge4_mask *out){
 for(unsigned i=0;i<4;i++)out->v[i]=(in->v[0][i]<limit[i]||in->v[1][i]<limit[i]||in->v[2][i]<limit[i])?UINT32_MAX:0;
}
#endif
