#!/usr/bin/env python3
"""Emit main/pocket/fir_pie.c: the MP3 downsample FIR as an eight-output PIE kernel.

The tap loop is unrolled because a 32-tap window walk is not loop-shaped: the
window address moves two bytes per tap, and `ee.ld.128.usar.ip` can only step a
pointer by a multiple of sixteen, so the two-byte walk is a core `addi` and the
window is read as the unaligned pair of aligned blocks that contain it
(docs/perf/pie-simd.md 1.4). Written out by this script rather than by hand so
the tap order, the coefficient walk and the pointer walks cannot drift apart.
"""
import os

TAPS = 32
HERE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))   # repository root

body = []
for k in range(TAPS):
    # The coefficient is double buffered: it is loaded one tap ahead of its use,
    # because a load's result is a stage-2 def and reading it in the next
    # instruction stalls for a cycle (docs/perf/pie-simd.md 2.2). The last tap
    # has no "next" coefficient to fetch, which is also what keeps the pointer
    # walk at exactly two bytes per tap.
    cur, nxt = ('q3', 'q4') if k % 2 == 0 else ('q4', 'q3')
    body += [
        '        "  ee.ld.128.usar.ip q0, %[p], 0\\n"',
        '        "  ee.ld.128.usar.ip q1, %[q], 0\\n"',
        '        "  ee.src.q q2, q0, q1\\n"',
    ]
    if k + 1 < TAPS:
        body.append('        "  ee.vldbc.16.ip %s, %%[h], 2\\n"' % nxt)
    body += [
        '        "  ee.vmulas.s16.qacc q2, %s\\n"' % cur,
        '        "  addi %[p], %[p], -2\\n"',
        '        "  addi %[q], %[q], -2\\n"',
    ]

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
@@BODY@@
        "  ee.srcmb.s16.qacc q0, %[sh14], 0\n"
        "  ee.vst.128.ip q0, %[out], 16\n"
        :
        : [p] "a"(window), [q] "a"((const char *)window + 16), [h] "a"(h), [out] "a"(out),
          [sh14] "a"(14)
        : "memory");
}
'''

src = HEAD.replace('@@TAPS@@', str(TAPS)).replace('@@BODY@@', '\n'.join(body))
out = os.path.join(HERE, "main", "pocket", "fir_pie.c")
with open(out, 'w', encoding='utf-8') as fh:
    fh.write(src)
print(f'wrote {out}: {len(body) // 7} taps unrolled, {len(body)} asm lines')
