#include "ksn_frost.h"
#include "ksn_stress_scene.h"
#include <stdio.h>
#include <string.h>
void reference_init(ksn_frost *);
ksn_result reference_feed(ksn_frost *,uint16_t,uint16_t,const uint16_t *);
ksn_result reference_blur(ksn_frost *,uint8_t);
ksn_result reference_span(const ksn_frost *,uint16_t,uint16_t,uint16_t,ksn_rgba,uint16_t *);
#define CHECK(x) do{if(!(x)){fprintf(stderr,"equivalence line %d\n",__LINE__);return 1;}}while(0)
static unsigned seed=17;
static unsigned random_word(void){seed=1664525u*seed+1013904223u;return seed;}
int main(void){
    ksn_frost a,b;uint16_t input[240*8],actual[256],expected[256];
    for(unsigned trial=0;trial<100;trial++){
        unsigned t=random_word();stress_frame scene=stress_frame_prepare(t);
        for(unsigned y=0;y<135;y++){
            stress_source_row(&scene,y,actual);
            for(unsigned x=0;x<240;x++)CHECK(actual[x]==stress_source(x,y,t));
        }
    }
    for(unsigned trial=0;trial<16;trial++){
        ksn_frost_init(&a);reference_init(&b);
        for(unsigned y=0;y<135;y+=8){
            unsigned rows=y==128?7:8;
            for(unsigned i=0;i<rows*240;i++)input[i]=(uint16_t)(random_word()>>8);
            CHECK(ksn_frost_feed(&a,y,rows,input)==reference_feed(&b,y,rows,input));
        }
        CHECK(memcmp(a.state.image,b.state.image,sizeof(a.state.image))==0);
        CHECK(ksn_frost_blur(&a,1+trial%2)==reference_blur(&b,1+trial%2));
        CHECK(memcmp(a.state.image,b.state.image,sizeof(a.state.image))==0);
        for(unsigned y=0;y<135;y++)for(unsigned alpha=0;alpha<256;alpha++){
            unsigned x=alpha%241,count=240-x,offset=1+(alpha&7);
            ksn_rgba tint=(random_word()&0xffffff00u)|alpha;
            for(unsigned i=0;i<256;i++)actual[i]=expected[i]=0xdead;
            CHECK(ksn_frost_span(&a,y,x,count,tint,actual+offset)==KSN_OK);
            CHECK(reference_span(&b,y,x,count,tint,expected+offset)==KSN_OK);
            CHECK(memcmp(actual,expected,sizeof(actual))==0);
        }
        for(unsigned x=0;x<=240;x++)for(unsigned count=0;count<=16&&count<=240-x;count++){
            for(unsigned i=0;i<256;i++)actual[i]=expected[i]=0xdead;
            unsigned offset=1+(x&7);ksn_rgba tint=random_word();
            CHECK(ksn_frost_span(&a,134,x,count,tint,actual+offset)==KSN_OK);
            CHECK(reference_span(&b,134,x,count,tint,expected+offset)==KSN_OK);
            CHECK(memcmp(actual,expected,sizeof(actual))==0);
        }
    }
    puts("frost equivalence: PASS (random input, both radii, all alpha, partial spans)");return 0;
}
