/* The MP3 downsample FIR as an eight-output PIE kernel.
 *
 * main/pocket/mp3_decode.c runs one output per decoded sample:
 *
 *     sum = sum(history[(cursor-k) and 31] * filter[k], k=0..31);
 *     sample = sum >> 14;  clamp to int16
 *
 * and this is the same filter, eight outputs at a time, one tap at a time:
 * eight lanes hold eight consecutive outputs, each tap broadcast from the
 * coefficient table multiplies the window under it and accumulates into the
 * eight-lane accumulator (`EE.VMULAS.S16.QACC`, TRM 1.8.160). The sum has no
 * rounding in it, so the order of the products cannot change the result, and the
 * saturating readout (`EE.SRCMB.S16.QACC`) is the C clamp: tools/pie/models/
 * fir_model.c proves both over 1.6M cases, and tools/pie/test_kernels.py proves
 * that this assembly is that arithmetic.
 *
 * The window of tap k starts two bytes below the window of tap k-1, and
 * `EE.LD.128.USAR.IP` can only move a pointer by a multiple of sixteen bytes
 * (the Espressif assembler rejects 2 and accepts 16 -- see tools/pie/piesim.py),
 * so the two-byte walk is a core `addi` and the window is read as the unaligned
 * pair: a USAR load of the aligned block that contains the window, a USAR load of
 * the next block, and `EE.SRC.Q` shifting the concatenation right by the byte
 * offset the load left in SAR_BYTE. The first operand of SRC.Q is the *low*
 * half; swapping the two reads the window sixteen bytes away, and
 * tools/pie/test_piesim.py asserts that failure direction explicitly.
 *
 * The caller owns the ring. The eight most recent samples must be resident, and
 * the windows reach seven samples back from the oldest output and seven forward
 * of the newest, so the buffer holds the ring twice (buf[t] and buf[t + 32] both
 * hold history[t mod 32]) and a block is computed after its whole group of eight
 * has been pushed. FIR_RING_SPAN is how many int16 that needs.
 */
#include <stdint.h>

#define FIR_TAPS  32
#define FIR_LANES 8

/* The ring twice, plus the window the oldest lane of the oldest output reaches. */
#define FIR_RING_SPAN (2 * FIR_TAPS + 2 * FIR_LANES)

/* The shipping scalar form, kept as the definition the kernel is checked
 * against (tools/pie/test_kernels.py mirrors it). */
__attribute__((unused))
static int16_t fir_scalar_sample(const int16_t *hist, int cursor, const int16_t *h) {
    int32_t sum = 0;
    for (unsigned k = 0; k < FIR_TAPS; k++)
        sum += (int32_t)hist[(cursor - (int)k) % FIR_TAPS] * h[k];
    int32_t v = sum >> 14;
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* Eight outputs at once. `window` is the byte address of the newest sample of the
 * OLDEST output, i.e. &ring[(cursor - 7) + FIR_TAPS] in the doubled buffer; the
 * kernel walks the taps downwards from there (tap 0 at the highest address,
 * matching the coefficient table's order). `h` is the Q14 table, `out` receives
 * eight int16 in time order (oldest first). */
__attribute__((noinline))
static void fir8_pie(const int16_t *window, const int16_t *h, int16_t *out) {
    __asm__ volatile(
        "  ee.zero.qacc\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q3, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vldbc.16.ip q4, %[h], 2\n"
        "  ee.vmulas.s16.qacc q2, q3\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.ld.128.usar.ip q0, %[p], 0\n"
        "  ee.ld.128.usar.ip q1, %[q], 0\n"
        "  ee.src.q q2, q0, q1\n"
        "  ee.vmulas.s16.qacc q2, q4\n"
        "  addi %[p], %[p], -2\n"
        "  addi %[q], %[q], -2\n"
        "  ee.srcmb.s16.qacc q0, %[sh14], 0\n"
        "  ee.vst.128.ip q0, %[out], 16\n"
        :
        : [p] "a"(window), [q] "a"((const char *)window + 16), [h] "a"(h), [out] "a"(out),
          [sh14] "a"(14)
        : "memory");
}
