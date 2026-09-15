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
 * Addressing, which is where the cost is (docs/perf/pie-opt-plan.md 6, A1):
 *
 *   - the window of tap k starts two bytes below the window of tap k-1, so the
 *     aligned 16-byte block containing it changes only every eight taps. Three
 *     block registers rotate, one new block is loaded per group of eight taps,
 *     and the pair a group needs is two of them;
 *   - the byte offset inside the block walks 0, 14, 12, ... 2 and repeats. It is
 *     one register stepping by -2 per tap: the shift comes from SAR[3:0], so the
 *     underflow wraps to zero on its own;
 *   - `EE.SRC.Q` concatenates the pair (the first operand is the *low* half) and
 *     shifts it right by that byte offset. Swapping the halves reads the window
 *     sixteen bytes away; tools/pie/test_piesim.py asserts that failure
 *     direction explicitly.
 *
 * That is five instructions per tap and six loads per eight outputs, and because
 * the shift is a whole number of bytes this version is bit-identical to the
 * scalar form -- the models and the instruction-level test are the contract.
 *
 * The caller owns the ring: it holds the ring twice (buf[t] and buf[t + 32] both
 * hold history[t mod 32]), it pushes whole groups of eight samples, and the
 * window start it passes must be 16-byte aligned -- true for every block once
 * the first one is, since the cursor advances by eight. FIR_RING_SPAN is how
 * many int16 the buffer needs.
 */
#include <stdint.h>

#define FIR_TAPS  32
#define FIR_LANES 8

/* The ring, held twice: buf[t] and buf[t + FIR_TAPS] both hold history[t % FIR_TAPS],
 * so an eight-sample window never wraps in the middle of a load. 64 int16 covers
 * every window the kernel reads (the oldest lane reaches six samples back from
 * the newest and the widest tap reaches thirty-one below that). */
#define FIR_RING_SPAN (2 * FIR_TAPS)

/* Push one sample into the doubled ring. `cursor` is where this sample goes, in
 * the ring's own 0..31 numbering; the second copy is what makes the window
 * contiguous. */
static inline void fir_ring_push(int16_t *ring, int cursor, int16_t sample) {
    ring[cursor] = sample;
    ring[cursor + FIR_TAPS] = sample;
}

/* The window the kernel wants for the eight outputs ending at `cursor`: the byte
 * address of the newest sample of the OLDEST output, i.e. &ring[cursor - 7 + FIR_TAPS].
 * It must land on a 16-byte boundary, which is a property of the ring's phase
 * rather than of this call: blocks advance by eight samples, so
 * (cursor + 25) % 8 is the same for every block once the first one is right
 * (cursor = 7, 15, 23, 31 ...). The firmware checks that phase once, when the
 * ring is set up -- FIR_PHASE_OK below -- because a wrong phase would make the
 * kernel read the wrong samples silently, with no fault to notice. */
static inline const int16_t *fir_window(const int16_t *ring, int cursor) {
    const int16_t *w = ring + cursor - (FIR_LANES - 1) + FIR_TAPS;
    /* No <assert.h> in the firmware's hot path: the caller checks the phase once. */
    return w;
}

/* The caller's phase rule, as a constant the ring setup can check: the newest
 * sample of a block has (cursor % 8) == 7. */
#define FIR_PHASE_OK(cursor) (((cursor) % FIR_LANES) == (FIR_LANES - 1))

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

/* Eight outputs at once. `window` is the 16-byte aligned byte address of the
 * newest sample of the OLDEST output, i.e. &ring[(cursor - 7) + FIR_TAPS] in the
 * doubled buffer; the taps are walked downwards from there, so tap 0 sits at the
 * highest address and matches the coefficient table's order. `out` receives
 * eight int16 in time order (oldest first). */
__attribute__((noinline))
static void fir8_pie(const int16_t *window, const int16_t *h, int16_t *out) {
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
