#include "../main/kobako/random_stream.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    const uint32_t expected[]={270369,67634689,2647435461u,307599695u,2398689233u};
    kobako_random_t a,b,zero,alias;
    kobako_random_init(&a,1);kobako_random_init(&b,1);
    for(unsigned i=0;i<5;i++)assert(kobako_random_next(&a)==expected[i]);
    assert(kobako_random_next(&b)==expected[0]);
    kobako_random_init(&zero,0);kobako_random_init(&alias,0x6D2B79F5u);
    for(unsigned i=0;i<10000;i++)assert(kobako_random_next(&zero)==kobako_random_next(&alias));
    kobako_random_init(&a,UINT32_MAX);b=a;
    for(unsigned i=0;i<100000;i++) {
        float f=kobako_random_float(&a);
        assert(f>=0&&f<1);
        assert((double)f==(kobako_random_next(&b)>>8)/16777216.0);
    }
    puts("RANDOM_OK: vectors, independent streams, zero seed, float parity/range");
}
