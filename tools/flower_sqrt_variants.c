// Three integer square roots for this part, read as [obj] rather than timed on
// the host (host microseconds say nothing here: docs/perf/pie-simd.md 3.13).
//
//   source /opt/esp-idf/export.sh
//   xtensa-esp32s3-elf-gcc -O2 -c tools/flower_sqrt_variants.c -o /tmp/sqrt.o
//   xtensa-esp32s3-elf-objdump -d --no-show-raw-insn /tmp/sqrt.o
//
// The question is whether the restoration loop's ~8-9 data-dependent steps (it
// prescales, so the step count follows the magnitude of x, not the precision)
// can be cut by unrolling, by going branchless, or by Newton with the LX7's
// integer divide. All three return floor(sqrt(x)) for x < 2^31 -- the correction
// at the end of the Newton version is two compares on values this scene reaches.
#include <stdint.h>

// A. The shape flower_isqrt_q() has now: one iteration per two bits, entered at
//    the highest set pair of bits (the prescale is the loop that shifts `bit`).
__attribute__((noinline)) uint32_t sqrt_restore(uint32_t x) {
    uint32_t rem = x, root = 0, bit = 1u << 30;
    while (bit > rem) bit >>= 2;
    while (bit) {
        if (rem >= root + bit) { rem -= root + bit; root = (root >> 1) + bit; }
        else root >>= 1;
        bit >>= 2;
    }
    return root;
}

// B. Same recurrence with the compare turned into a mask, so there is no branch
//    inside the loop at all. The seed search replaces A's prescale loop with a
//    leading-zero count (NSA, one instruction) and a shift.
__attribute__((noinline)) uint32_t sqrt_restore_nobranch(uint32_t x) {
    if (!x) return 0;
    uint32_t bit = 1u << ((31 - (uint32_t)__builtin_clz(x)) & ~1u);
    uint32_t rem = x, root = 0;
    for (; bit; bit >>= 2) {
        uint32_t t = root + bit;
        uint32_t m = (uint32_t)0 - (uint32_t)(rem >= t);
        rem -= t & m;
        root = (root >> 1) + (bit & m);
    }
    return root;
}

// C. Newton: seed 2^ceil(e/2) is within a factor of sqrt(2) of the answer, and
//    each pass doubles the number of correct bits, so three passes take a
//    1-bit-accurate seed past 8. The divide is QUOU -- one instruction whose
//    latency varies with the operands, which is what [obj] cannot show.
__attribute__((noinline)) uint32_t sqrt_newton(uint32_t x) {
    if (!x) return 0;
    uint32_t e = (uint32_t)(31 - __builtin_clz(x));
    uint32_t r = (uint32_t)1 << ((e + 1) >> 1);
    r = (r + x / r) >> 1;
    r = (r + x / r) >> 1;
    r = (r + x / r) >> 1;
    while (r && r * r > x) r--;                  // x < 2^31 keeps r*r in 32 bits
    while ((r + 1) * (r + 1) <= x && r < 65535) r++;
    return r;
}

// D. The restoration loop unrolled by three, with `bit` still a variable (the
//    prescale decides where to start, so the steps cannot all be constants).
//    Each step is the same five instructions; what unrolling removes is one loop
//    control per step -- the question is whether one instruction out of seven is
//    worth answering.
#define RESTORE_STEP(BIT) do { uint32_t t=root+(BIT); if(rem>=t){rem-=t;root=(root>>1)+(BIT);}else root>>=1; bit>>=2; } while(0)
__attribute__((noinline)) uint32_t sqrt_restore_unroll3(uint32_t x) {
    uint32_t rem = x, root = 0, bit = 1u << 30;
    while (bit > rem) bit >>= 2;
    while (bit) {
        RESTORE_STEP(bit);
        if (!bit) break;
        RESTORE_STEP(bit);
        if (!bit) break;
        RESTORE_STEP(bit);
    }
    return root;
}

// E. All sixteen steps, no loop at all and no prescale: the cost is fixed at
//    sixteen steps whatever x is, which is *more* than the loop pays for the
//    values this scene reaches. It is here as the upper bound of "just unroll
//    it", not as a candidate.
#define RESTORE_FIXED(BIT) do { uint32_t t=root+(BIT); if(rem>=t){rem-=t;root=(root>>1)+(BIT);}else root>>=1; } while(0)
__attribute__((noinline)) uint32_t sqrt_restore_unroll16(uint32_t x) {
    uint32_t rem = x, root = 0;
    RESTORE_FIXED(1u<<30); RESTORE_FIXED(1u<<28); RESTORE_FIXED(1u<<26); RESTORE_FIXED(1u<<24);
    RESTORE_FIXED(1u<<22); RESTORE_FIXED(1u<<20); RESTORE_FIXED(1u<<18); RESTORE_FIXED(1u<<16);
    RESTORE_FIXED(1u<<14); RESTORE_FIXED(1u<<12); RESTORE_FIXED(1u<<10); RESTORE_FIXED(1u<<8);
    RESTORE_FIXED(1u<<6); RESTORE_FIXED(1u<<4); RESTORE_FIXED(1u<<2); RESTORE_FIXED(1u);
    return root;
}

int main(void) {                       // a driver, so the file builds as a program too
    volatile uint32_t a = sqrt_restore(123456u), b = sqrt_restore_nobranch(123456u),
                      c = sqrt_newton(123456u), d = sqrt_restore_unroll3(123456u),
                      e = sqrt_restore_unroll16(123456u);
    return (int)(a + b + c + d + e);
}
