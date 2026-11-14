#ifndef KSN_BAND_BITS_H
#define KSN_BAND_BITS_H
#include <stdint.h>
/* PR 133 ordered bitmap iterator, shared by Shiki entries and native band providers.
 * Inputs must be stable expressions; caller validates bounds and bitmap lifetime. */
#define KSN_BAND_FOR(j, bits, first, end) \
    for(unsigned bw_=first>>5,bwe_=end?((end-1u)>>5):0u;first<end&&bw_<=bwe_;bw_++) \
        for(uint32_t bm_=bits[bw_] \
                &(bw_==(first>>5)?(0xffffffffu<<(first&31u)):0xffffffffu) \
                &(bw_==bwe_&&(end&31u)?(0xffffffffu>>(32u-(end&31u))):0xffffffffu); \
            bm_;bm_&=bm_-1u) \
            for(unsigned j=bw_*32u+(unsigned)__builtin_ctz(bm_),once_=1;once_;once_=0)
#endif
