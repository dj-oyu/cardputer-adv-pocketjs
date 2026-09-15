// The host stand-in for the PIE kernel, for tools/test_mp3.sh only.
//
// The assembly cannot assemble off-Xtensa, so this file implements the same
// contract in C: eight outputs, the same arithmetic-shift floor and the same
// clamp, one sixteen-bit result per lane. Running main/pocket/mp3_decode.c's
// block path on the host is the point: the two arms of g_mp3_fir_pie must emit
// the same samples bit for bit, which is what tools/test_mp3.c compares.
//
// It also checks the alignment the kernel needs -- the window on a 16-byte
// boundary -- so a caller that gets the ring's phase wrong fails loudly here
// instead of silently reading the wrong samples on the device.
#include "fir_pie.h"
#include <assert.h>
#include <stdint.h>

void fir8_pie(const int16_t *window, const int16_t *h, int16_t *out) {
    // The kernel's own requirement: fir_window must land on a 16-byte boundary.
    assert(((uintptr_t)window & 15u) == 0);
    for (int j = 0; j < FIR_LANES; j++) {
        int32_t sum = 0;
        for (int k = 0; k < FIR_TAPS; k++) {
            // Lane j of tap k: the window walks forward in time (lane 0 is the oldest
            // output) while the taps walk backward, so the index is j - k. It goes
            // negative for the older taps, and those addresses are the ring's second
            // copy -- `window` sits FIR_RING_SLOTS above the ring's start, so the wrap
            // is memory rather than arithmetic.
            sum += (int32_t)window[j - k] * h[k];
        }
        int32_t v = sum >> 14;
        out[j] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}
