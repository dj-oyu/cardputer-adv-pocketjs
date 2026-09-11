#include "../main/pocket/random_stream.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    const uint32_t expected[]={270369,67634689,2647435461u,307599695u,2398689233u};
    pocket_random_t a,b,zero,alias;
    pocket_random_init(&a,1);pocket_random_init(&b,1);
    for(unsigned i=0;i<5;i++)assert(pocket_random_next(&a)==expected[i]);
    assert(pocket_random_next(&b)==expected[0]);
    pocket_random_init(&zero,0);pocket_random_init(&alias,0x6D2B79F5u);
    for(unsigned i=0;i<10000;i++)assert(pocket_random_next(&zero)==pocket_random_next(&alias));
    pocket_random_init(&a,UINT32_MAX);b=a;
    for(unsigned i=0;i<100000;i++) {
        float f=pocket_random_float(&a);
        assert(f>=0&&f<1);
        assert((double)f==(pocket_random_next(&b)>>8)/16777216.0);
    }
    puts("RANDOM_OK: vectors, independent streams, zero seed, float parity/range");
}
