#include <stdint.h>
/* Generic 3-edge/4-lane update. All planes aligned16 and disjoint.
 h[12],l[12],d[24]: three edge planes, each d edge has dl[4],dh[4].
 m[4]=4095. out[4] is OR of outside masks. CP3 owner required.
 q0..q7 caller-saved, separate TU/no LTO. No SAR/QACC use. */
#define EDGE_STEP \
 "ee.vld.128.ip q1, %[l], 0\n" \
 "ee.vld.128.ip q3, %[d], 16\n" \
 "ee.vld.128.ip q0, %[h], 0\n" \
 "ee.vadds.s32.ld.incp q2, %[d], q1, q1, q3\n" \
 "ee.vcmp.gt.s32 q5, q1, q4\n" \
 "ee.vadds.s32 q0, q0, q2\n" \
 "ee.andq q1, q1, q4\n" \
 "ee.vsubs.s32.st.incp q1, %[l], q0, q0, q5\n" \
 "ee.vcmp.lt.s32 q5, q0, q6\n" \
 "ee.orq q7, q7, q5\n" \
 "ee.vst.128.ip q0, %[h], 16\n"
void edge3x4_sched_step(int32_t*h,int32_t*l,const int32_t*d,const int32_t*m,int32_t*out){
 __asm__ volatile(
 "ee.vld.128.ip q4, %[m], 0\n"
 "ee.zero.q q6\n"
 "ee.zero.q q7\n"
 EDGE_STEP EDGE_STEP EDGE_STEP
 "ee.vst.128.ip q7, %[out], 0\n"
 :[h]"+&a"(h),[l]"+&a"(l),[d]"+&a"(d)
 :[m]"a"(m),[out]"a"(out):"memory");
}
