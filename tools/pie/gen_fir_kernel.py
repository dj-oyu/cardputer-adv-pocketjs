#!/usr/bin/env python3
"""Emit main/pocket/fir_pie.c and fir_pie.h: the MP3 downsample FIR as an eight-output PIE kernel.

The tap loop is unrolled because a 32-tap window walk is not loop-shaped. It is
written out by this script rather than by hand so the tap order, the coefficient
walk, the block rotation and the shift walk cannot drift apart.

Two things make it cheap (docs/perf/pie-opt-plan.md 6, A1):

  * the window of tap k starts two bytes below the window of tap k-1, so the
    aligned 16-byte block that *contains* the window only changes every eight
    taps. The pair of blocks a group needs is two of three registers that rotate,
    and six loads cover eight outputs;
  * the byte offset inside that block walks 0, 14, 12, ... 2 and repeats, which
    is one register stepping by -2 per tap: the shift comes from SAR[3:0], so the
    underflow wraps to zero on its own.

Per tap that leaves wsr.sar, addi, EE.SRC.Q, EE.VLDBC.16.IP and
EE.VMULAS.S16.QACC. The shift is exact (whole bytes), so the kernel is
bit-identical to the scalar form; tools/pie/test_kernels.py checks that, and
tools/pie/models/fir_model.c proves the arithmetic and the accumulator range.
"""
import os

TAPS = 32
LANES = 8
HERE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))   # repository root

# The registers the generated loop uses.
BLK = ('q5', 'q6', 'q7')      # the three rotating blocks; index 0 holds the highest address
WIN = 'q2'                    # the window an EE.SRC.Q produces
COEF = ('q3', 'q4')           # the broadcast coefficient, double buffered

body = []


def load_block(reg, first=False):
    """Load the next aligned block into `reg`. The pointer walks downwards one block
    at a time; EE.VLD.128.IP's own immediate is not used for that walk, so the
    kernel does not depend on a negative immediate being accepted."""
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
    # the group index is (k + 7) // 8. The pair is (B(g), B(g-1)) with the low half
    # first, and a group from 2 on needs one new block, loaded into the register
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

HDR = r'''/* The MP3 downsample FIR kernel's interface: the ring it reads, the phase it
 * needs, and the entry point. The implementation -- and the reasoning behind the
 * addressing -- is in fir_pie.c, which tools/pie/gen_fir_kernel.py generates from
 * the same source as this header. */
#ifndef FIR_PIE_H
#define FIR_PIE_H

#include <stdint.h>

#define FIR_TAPS  @@TAPS@@
#define FIR_LANES 8

/* The ring's history is longer than the filter: FIR_TAPS + FIR_LANES slots. Eight
 * outputs are computed at a time, so the block's own eight writes land in slots that
 * "thirty-one steps back from the first output" would otherwise alias -- with a
 * 32-slot ring the kernel would read this block's later samples where it needs the
 * previous frame's tail. Forty slots make the block's writes disjoint from every
 * sample it reads; 2*(32+8) is eighty int16.
 *
 * The ring is held twice: ring[t] and ring[t + FIR_RING_SLOTS] both hold
 * history[t % FIR_RING_SLOTS], so a block's window never wraps in the middle of a
 * load. The caller's `history` is this buffer, and it must be 16-byte aligned: the
 * window the kernel is handed has to land on a 16-byte boundary, and the phase rule
 * below only gets it there when the ring itself starts aligned. */
#define FIR_RING_SLOTS (FIR_TAPS + FIR_LANES)
#define FIR_RING_SPAN (2 * FIR_RING_SLOTS)

/* Where one sample goes: both copies, which is what makes a window contiguous. */
static inline void fir_ring_push(int16_t *ring, int slot, int16_t sample) {
    ring[slot] = sample;
    ring[slot + FIR_RING_SLOTS] = sample;
}

/* The window for the eight outputs that start at `block_start` (the ring slot where the
 * block's eight samples begin, so lane 0 is the oldest output). It has to land on a
 * 16-byte boundary, which is a property of the ring's phase rather than of this call:
 * blocks advance by eight slots and a block starts where (block_start % 8) == 0, so the
 * alignment holds for every block once the first one is right. FIR_PHASE_OK is the
 * check the ring setup runs once, because a wrong phase makes the kernel read the wrong
 * samples silently -- there is no fault to notice.
 *
 * For block_start in {0, 8, 16, 24, 32} the taps reach ring[block_start + 9] at the
 * lowest and ring[block_start + 47] at the highest, which is what FIR_RING_SPAN has to
 * cover. */
static inline const int16_t *fir_window(const int16_t *ring, int block_start) {
    return ring + block_start + FIR_RING_SLOTS;
}

/* The phase rule: a block starts where block_start % 8 == 0. */
#define FIR_PHASE_OK(block_start) (((block_start) % FIR_LANES) == 0)

/* Eight outputs for the eight most recent samples: `window` from fir_window, `h` the
 * Q14 coefficient table, `out` eight int16 in time order (oldest first). */
void fir8_pie(const int16_t *window, const int16_t *h, int16_t *out);

#endif /* FIR_PIE_H */
'''

HEAD = r'''/* The MP3 downsample FIR as an eight-output PIE kernel.
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
@@BODY@@
        "  ee.srcmb.s16.qacc q0, %[sh14], 0\n"
        "  ee.vst.128.ip q0, %[out], 16\n"
        :
        : [p] "a"((const char *)window + 16), [h] "a"(h), [out] "a"(out),
          [sh] "a"(0), [sh14] "a"(14)
        : "memory");
}
'''

hdr = HDR.replace('@@TAPS@@', str(TAPS))
src = HEAD.replace('@@BODY@@', '\n'.join(body))
for name, text in (('fir_pie.h', hdr), ('fir_pie.c', src)):
    path = os.path.join(HERE, 'main', 'pocket', name)
    with open(path, 'w', encoding='utf-8') as fh:
        fh.write(text)
    print(f'wrote {path}')
print(f'  {len(body)} asm lines: 5 per tap + 1 block load per group')
