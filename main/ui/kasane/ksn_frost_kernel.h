#ifndef KSN_FROST_KERNEL_H
#define KSN_FROST_KERNEL_H
#include "ksn_frost.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(KSN_FROST_PIE_MODEL)
#define KSN_FROST_VECTOR 1
/* Task context only (coprocessor 3). k[0..3]=inverse alpha,1,128,SAR;
 * then three (left,right,tint numerator,mask,packing multiplier) tuples.
 * left/right <=4080. Rounding is exactly the scalar two-stage definition.
 * For 0<=n<=65152, floor(n/255)=(n+1+(n>>8))>>8. No signed overflow or
 * QACC saturation: interpolated channels and tint results are <=255. */
static void ksn_frost_cell_pie(uint16_t *dst,const uint16_t *k){
#ifdef CONFIG_IDF_TARGET_ESP32S3
    static const uint16_t weights[16] __attribute__((aligned(16)))={
        15,13,11,9,7,5,3,1, 1,3,5,7,9,11,13,15
    };
    const uint16_t *wp=weights,*kp=k;
    unsigned channels=3,sh8=8,sar=3;
    __asm__ volatile(
        "wsr.sar %[sar]\n"
        "ee.vld.128.ip q0, %[wp], 16\n"
        "ee.vld.128.ip q1, %[wp], 16\n"
        "ee.vldbc.16.ip q7, %[kp], 2\n" /* inverse alpha */
        "ee.vldbc.16.ip q5, %[kp], 2\n" /* one */
        "ee.zero.q q6\n"
        "addi %[kp], %[kp], 4\n"
        "1:\n"
        "ee.vldbc.16.ip q2, %[kp], 2\n"
        "ee.vldbc.16.ip q3, %[kp], 2\n"
        "ee.zero.qacc\n"
        "ee.vmulas.u16.qacc q2, q0\n"
        "ee.vmulas.u16.qacc q3, q1\n"
        "ee.vldbc.16 q4, %[round]\n"
        "ee.vmulas.u16.qacc q4, q5\n"
        "ee.srcmb.s16.qacc q2, %[sh8], 0\n"
        "ee.vldbc.16.ip q4, %[kp], 2\n" /* tint numerator */
        "ee.zero.qacc\n"
        "ee.vmulas.u16.qacc q2, q7\n"
        "ee.vmulas.u16.qacc q4, q5\n"
        "ee.srcmb.s16.qacc q3, %[sh8], 0\n" /* QACC now n>>8 */
        "ee.vmulas.u16.qacc q2, q7\n"
        "ee.vmulas.u16.qacc q4, q5\n"
        "ee.vmulas.u16.qacc q5, q5\n"
        "ee.srcmb.s16.qacc q2, %[sh8], 0\n"
        "ee.vldbc.16.ip q3, %[kp], 2\n" /* mask */
        "ee.vldbc.16.ip q4, %[kp], 2\n" /* multiplier, SAR=3 */
        "ee.andq q2, q2, q3\n"
        "ee.vmul.u16 q2, q2, q4\n"
        "ee.orq q6, q6, q2\n"
        "addi %[n], %[n], -1\n"
        "bnez %[n], 1b\n"
        "ee.vst.128.ip q6, %[dst], 16\n"
        : [wp] "+&a"(wp), [kp] "+&a"(kp), [dst] "+&a"(dst), [n] "+&a"(channels)
        : [round] "a"(k+2), [sh8] "a"(sh8), [sar] "a"(sar)
        : "memory");
#else
    /* Wrapper tests on a host. Assembly itself is checked by piesim and the
     * on-device A/B diagnostic, not by this model. */
    for(unsigned i=0;i<8;i++){
        unsigned pixel=0;
        for(unsigned c=0;c<3;c++){
            const uint16_t *p=k+4+c*5;
            unsigned v=(p[0]*(15-2*i)+p[1]*(1+2*i)+128)>>8;
            unsigned n=v*k[0]+p[2];
            v=(n+1+(n>>8))>>8;
            pixel|=((v&p[3])*p[4])>>3;
        }
        dst[i]=(uint16_t)pixel;
    }
#endif
}
#else
#define KSN_FROST_VECTOR 0
#endif
#endif
