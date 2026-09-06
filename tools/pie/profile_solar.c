// Development-only workload: no device, SPI, menu, or filesystem in timings.
// Compile with -O2 for host timing; -pg -fno-inline -fno-ipa-cp -fno-builtin
// for gprof call counts. Host elapsed time is NOT an ESP32 cycle estimate.
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#ifndef SOLAR_SOURCE
#define SOLAR_SOURCE "../../main/solar_sail.c"
#endif
#include SOLAR_SOURCE
#include "../../main/solar_time.c"
static uint64_t ns(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;
}
int main(int argc,char **argv) {
    int frames=argc>1?atoi(argv[1]):2880;
    if(frames<1)return 2;
    uint16_t frame[W*H] __attribute__((aligned(16)));
    solar_sail_prepare(0,0,0); // Cold init deliberately outside steady-state.
    uint64_t prep=0,draw=0,hash=1469598103934665603ULL,primitives=0;
    unsigned peak=0;
    for(int n=0;n<frames;n++) {
        elapsed=n*288.0/frames;
        int tx=(n/37)%3*180-180,ty=(n/53)%3*180-180;
        uint64_t t=ns();solar_sail_prepare(1.0f/30,tx,ty);prep+=ns()-t;
        primitives+=count;if(count>peak)peak=count;
        t=ns();
        for(int y=0;y<H;y+=8)solar_sail_draw(frame+y*W,y,H-y<8?H-y:8);
        draw+=ns()-t;
        for(int p=0;p<W*H;p++){hash^=frame[p];hash*=1099511628211ULL;}
    }
    printf("frames=%d prep_us=%.3f draw_us=%.3f primitives_avg=%.2f peak=%u hash=%016llx\n",
        frames,prep/(frames*1000.0),draw/(frames*1000.0),primitives/(double)frames,peak,(unsigned long long)hash);
}
