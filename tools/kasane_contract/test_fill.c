#include "../../main/ui/kasane/ksn_render.c"
#include <assert.h>
#include <stdio.h>

static _Alignas(16) uint16_t guarded[1944];
int main(void){
    /* Every legal uint16 alignment, zero length, every tail, and a full strip.
     * The extra guards make address rounding or a partial vector store visible. */
    const uint16_t colors[]={0,1,0xffff,0x8000,0x07e0,0x001f,0xabcd};
    for(unsigned offset=0;offset<8;offset++)for(unsigned count=0;count<=1920;count++)
    for(unsigned c=0;c<sizeof(colors)/sizeof(colors[0]);c++){
        for(unsigned i=0;i<1944;i++)guarded[i]=0x5a5a;
        fill565(guarded+8+offset,count,colors[c]);
        for(unsigned i=0;i<1944;i++)
            assert(guarded[i]==(i>=8+offset&&i<8+offset+count?colors[c]:0x5a5a));
    }
    puts("RGB565 fill: PASS (all alignments/lengths, short tails, guards)");
}
