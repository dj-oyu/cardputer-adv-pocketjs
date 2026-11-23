/* The MP3 downsample FIR kernel's interface: the ring it reads, the phase it
 * needs, and the entry point. The implementation -- and the reasoning behind the
 * addressing -- is in fir_pie.c, which tools/pie/gen_fir_kernel.py generates from
 * the same source as this header. */
#ifndef FIR_PIE_H
#define FIR_PIE_H

#include <stdint.h>

#define FIR_TAPS  32
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
