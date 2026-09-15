/* The MP3 downsample FIR as an eight-output PIE kernel.
 *
 * main/pocket/mp3_decode.c runs one output per decoded sample:
 *
 *     sum = sum(history[(cursor-k) and 31] * filter[k], k=0..31);
 *     sample = sum >> 14;  clamp to int16
 *
 * and this is the same filter, eight outputs at a time, one tap at a time: eight
 * lanes hold eight consecutive outputs, each tap broadcast from the coefficient
 * table multiplies the window under it and accumulates into the eight-lane
 * accumulator (EE.VMULAS.S16.QACC, TRM 1.8.160). The sum has no rounding in it, so
 * the order of the products cannot change the result, and the saturating readout
 * (EE.SRCMB.S16.QACC) is the C clamp: tools/pie/models/fir_model.c proves both
 * over 1.6M cases and tools/pie/test_kernels.py proves that this assembly is that
 * arithmetic.
 *
 * Addressing, which is where the cost is (docs/perf/pie-opt-plan.md 6, A1):
 *
 *   - the window of tap k starts two bytes below the window of tap k-1, so the
 *     aligned 16-byte block containing it changes only every eight taps. Three
 *     block registers rotate, one new block is loaded per group, and the pair a
 *     group needs is two of them; six loads cover eight outputs;
 *   - the byte offset inside the block walks 0, 14, 12, ... 2 and repeats. It is
 *     one register stepping by -2 per tap, and because the shift comes from
 *     SAR[3:0] the underflow wraps to zero on its own;
 *   - EE.SRC.Q concatenates the pair (the first operand is the *low* half) and
 *     shifts it right by that byte offset. Swapping the halves reads the window
 *     sixteen bytes away; tools/pie/test_piesim.py asserts that failure direction
 *     explicitly.
 *
 * That is five instructions per tap and six loads per eight outputs, and because
 * the shift is a whole number of bytes this kernel is bit-identical to the scalar
 * form -- the models and the instruction-level test are the contract.
 *
 * The ring and the phase the kernel needs are documented in fir_pie.h, which is
 * generated with this file.
 */
#include "fir_pie.h"

/* The shipping scalar form, kept as the definition the kernel is checked against
 * (tools/pie/test_kernels.py mirrors it). */
__attribute__((unused))
static int16_t fir_scalar_sample(const int16_t *hist, int cursor, const int16_t *h) {
    int32_t sum = 0;
    for (unsigned k = 0; k < FIR_TAPS; k++)
        sum += (int32_t)hist[(cursor - (int)k) % FIR_TAPS] * h[k];
    int32_t v = sum >> 14;
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

__attribute__((noinline))
void fir8_pie(const int16_t *window, const int16_t *h, int16_t *out) {
    __asm__ volatile(
        "  ee.zero.qacc\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vld.128.ip q5, %[p], 0\n"
        "  addi %[p], %[p], -16\n"
        "  ee.vld.128.ip q6, %[p], 0\n"
        "  addi %[p], %[p], -16\n"
        "  ee.vld.128.ip q7, %[p], 0\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q6, q5\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[sh], %[sh], -2\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q7, q6\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[sh], %[sh], -2\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q7, q6\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[sh], %[sh], -2\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q7, q6\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[sh], %[sh], -2\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q7, q6\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[sh], %[sh], -2\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q7, q6\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[sh], %[sh], -2\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q7, q6\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[sh], %[sh], -2\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q7, q6\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[sh], %[sh], -2\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q7, q6\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[sh], %[sh], -2\n"
        "  addi %[p], %[p], -16\n"
        "  ee.vld.128.ip q5, %[p], 0\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q5, q7\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[sh], %[sh], -2\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q5, q7\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[sh], %[sh], -2\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q5, q7\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[sh], %[sh], -2\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q5, q7\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[sh], %[sh], -2\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q5, q7\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[sh], %[sh], -2\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q5, q7\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[sh], %[sh], -2\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q5, q7\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[sh], %[sh], -2\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q5, q7\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[sh], %[sh], -2\n"
        "  addi %[p], %[p], -16\n"
        "  ee.vld.128.ip q6, %[p], 0\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q6, q5\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[sh], %[sh], -2\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q6, q5\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[sh], %[sh], -2\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q6, q5\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[sh], %[sh], -2\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q6, q5\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[sh], %[sh], -2\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q6, q5\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[sh], %[sh], -2\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q6, q5\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[sh], %[sh], -2\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q6, q5\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[sh], %[sh], -2\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q6, q5\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[sh], %[sh], -2\n"
        "  addi %[p], %[p], -16\n"
        "  ee.vld.128.ip q7, %[p], 0\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q7, q6\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[sh], %[sh], -2\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q7, q6\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[sh], %[sh], -2\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q7, q6\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[sh], %[sh], -2\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q7, q6\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[sh], %[sh], -2\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q7, q6\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[sh], %[sh], -2\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q7, q6\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[sh], %[sh], -2\n"
        "  wsr.sar %[sh]\n"
        "  ee.src.q q2, q7, q6\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[sh], %[sh], -2\n"
        "  ee.srcmb.s16.qacc q0, %[sh14], 0\n"
        "  ee.vst.128.ip q0, %[out], 16\n"
        :
        : [p] "a"((const char *)window + 16), [h] "a"(h), [out] "a"(out),
          [sh] "a"(0), [sh14] "a"(14)
        : "memory");
}
