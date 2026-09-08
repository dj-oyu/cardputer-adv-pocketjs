// The two builds of GARDEN_MOTE_INDEX, held to the same output -- and a host
// timing that is here mainly to show that it says nothing.
//
//   gcc -O2 -DGARDEN_MOTE_INDEX=1 tools/garden_bench.c -lm -o /tmp/gb1
//   gcc -O2 -DGARDEN_MOTE_INDEX=0 tools/garden_bench.c -lm -o /tmp/gb0
//   /tmp/gb1 d > /tmp/d1; /tmp/gb0 d > /tmp/d0; cmp /tmp/d1 /tmp/d0
//
// `d` dumps 40 frames of the pixel half; the two builds must be byte
// identical, because the index is only allowed to be a faster way of asking
// which particles touch a row.
//
// Without `d` it times the same loop, and on x86 the two are the same to
// within noise -- 1,890 well-predicted branches over a struct that fits in L1
// cost nothing here and cost 1.0 ms on the board. The host cannot measure this
// change; it can only prove the change is invisible. See docs/pie-simd.md 3.8.
#include "../main/scene/garden.c"
#include <stdio.h>
#include <time.h>
int main(int argc,char**argv){
    static uint16_t fb[135*240];
    GardenFrame g={0};
    for(int w=0;w<16;w++)garden_prepare(&g,w*0.04f);
    if(argc>1&&argv[1][0]=='d'){
        for(int fr=0;fr<40;fr++){
            garden_prepare(&g,fr*0.37f);
            for(int y=0;y<135;y++)garden_pixels_row(fb+y*240,y,&g);
            fwrite(fb,sizeof fb,1,stdout);
        }
        return 0;
    }
    struct timespec a,b;
    const int F=400;
    clock_gettime(CLOCK_MONOTONIC,&a);
    for(int fr=0;fr<F;fr++){
        garden_prepare(&g,fr*0.37f);
        for(int y=0;y<135;y++)garden_pixels_row(fb+y*240,y,&g);
    }
    clock_gettime(CLOCK_MONOTONIC,&b);
    double ms=((b.tv_sec-a.tv_sec)*1e3+(b.tv_nsec-a.tv_nsec)/1e6)/F;
    fprintf(stderr,"index=%d  %.3f ms/frame\n",GARDEN_MOTE_INDEX,ms);
    return 0;
}
