#!/usr/bin/env python3
"""Emit main/pocket/fir_pie.c: the MP3 downsample FIR as an eight-output PIE kernel.

The tap loop is unrolled because a 32-tap window walk is not loop-shaped. It is
written out by this script rather than by hand so the tap order, the coefficient
walk, the block rotation and the shift walk cannot drift apart.

Two things make it cheap (docs/perf/pie-opt-plan.md 6, A1):

  * the window of tap k starts two bytes below the window of tap k-1, so the
    aligned 16-byte block that *contains* the window only changes every eight
    taps. One load per group of eight taps therefore covers the whole group, and
    the pair of blocks a group needs is two of three registers that rotate;
  * the byte offset inside that block walks 0, 14, 12, ... 2 and repeats, which
    is one register that steps by -2 per tap: the hardware takes SAR[3:0] as the
    byte offset, so the underflow at -16 wraps to 0 by itself.

Per tap that leaves wsr.sar, addi, EE.SRC.Q, EE.VLDBC.16.IP and
EE.VMULAS.S16.QACC -- five instructions, and 6 loads per eight outputs instead
of 64. The shift is exact (whole bytes), so this version is still bit-identical
to the scalar form; tools/pie/test_kernels.py checks that on every run.

The caller must keep the window start 16-byte aligned: the ring is written twice
(buf[t] and buf[t + 32]) and the caller pushes whole groups of eight, so
(cursor + 25) is a multiple of eight for every block -- one phase chosen when the
ring starts, and it stays true.
"""
import os

TAPS = 32
LANES = 8
HERE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))   # repository root

# The registers the generated loop uses.
BLK = ('q5', 'q6', 'q7')      # the three rotating blocks; index 0 is the highest address
WIN = 'q2'                    # the window an EE.SRC.Q produces
COEF = ('q3', 'q4')           # the broadcast coefficient, double buffered

body = []


def load_block(reg, first=False):
    """Load the next aligned block into `reg`. The pointer walks downwards by one
    block at a time, and EE.VLD.128.IP's immediate is not used for that walk so
    the kernel does not depend on a negative immediate being accepted."""
    if first:
        body.append('        "  ee.vld.128.ip %s, %%[p], 0\\n"' % reg)
    else:
        body.append('        "  addi %[p], %[p], -16\\n"')
        body.append('        "  ee.vld.128.ip %s, %%[p], 0\\n"' % reg)


# Prologue: the three blocks the first group needs, highest address first.
load_block(BLK[0], first=True)          # B(-1)
load_block(BLK[1])                      # B(0)
load_block(BLK[2])                      # B(1)

for k in range(TAPS):
    # The window's block pair changes when the window start crosses a 16-byte
    # boundary, which happens at taps 1, 9, 17, 25 ... -- not at 8, 16, 24 -- so
    # the group index is (k + 7) // 8. The pair is (B(g), B(g-1)) with the low
    # half first, and group g >= 2 needs a new block, loaded into the register
    # that becomes its low half.
    group = (k + LANES - 1) // LANES
    if k > 0 and (k - 1) % LANES == 0 and group >= 2:
        load_block(BLK[(group + 1) % 3])
    low, high = BLK[(group + 1) % 3], BLK[group % 3]
    cur, nxt = COEF[k % 2], COEF[(k + 1) % 2]
    body.append('        "  wsr.sar %[sh]\\n"')
    body.append('        "  ee.src.q %s, %s, %s\\n"' % (WIN, low, high))
    if k + 1 < TAPS:
        body.append('        "  ee.vldbc.16.ip %s, %%[h], 2\\n"' % nxt)
    body.append('        "  ee.vmulas.s16.qacc %s, %s\\n"' % (WIN, cur))
    body.append('        "  addi %[sh], %[sh], -2\\n"')

HEAD = r'''/* The MP3 downsample FIR as an eight-output PIE kernel.
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

#define FIR_TAPS  @@TAPS@@
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

/* Eight outputs at once. `window` is the 16-byte aligned byte address of the
 * newest sample of the OLDEST output, i.e. &ring[(cursor - 7) + FIR_TAPS] in the
 * doubled buffer; the taps are walked downwards from there, so tap 0 sits at the
 * highest address and matches the coefficient table's order. `out` receives
 * eight int16 in time order (oldest first). */
__attribute__((noinline))
static void fir8_pie(const int16_t *window, const int16_t *h, int16_t *out) {
    __asm__ volatile(
        "  ee.zero.qacc\n"
        "  ee.vldbc.16.ip __COEF0__, %[h], 2\n"
@@BODY@@
        "  ee.srcmb.s16.qacc q0, %[sh14], 0\n"
        "  ee.vst.128.ip q0, %[out], 16\n"
        :
        : [p] "a"((const char *)window + 16), [h] "a"(h), [out] "a"(out),
          [sh] "a"(0), [sh14] "a"(14)
        : "memory");
}
'''

src = (HEAD.replace('@@TAPS@@', str(TAPS))
           .replace('@@BODY@@', '\n'.join(body))
           .replace("__COEF0__", COEF[0]))
out_path = os.path.join(HERE, 'main', 'pocket', 'fir_pie.c')
with open(out_path, 'w', encoding='utf-8') as fh:
    fh.write(src)
print(f'wrote {out_path}: {len(body)} asm lines, 5 per tap + 1 block load per group')
