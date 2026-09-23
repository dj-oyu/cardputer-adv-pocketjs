#include <assert.h>
#include <stdio.h>
#define KASANE_P0_PROBE 1
#include "../../main/ui/kasane/ksn_p0_probe.c"

int main(void){
    ksn_p0_probe_reset();
    for(unsigned i=0;i<970;i++)ksn_p0_probe_sample(KSN_P0_OVERLAY_DRAW,1000);
    for(unsigned i=0;i<20;i++)ksn_p0_probe_sample(KSN_P0_OVERLAY_DRAW,20000);
    for(unsigned i=0;i<10;i++)ksn_p0_probe_sample(KSN_P0_OVERLAY_DRAW,40000);
    const series *s=&samples[KSN_P0_OVERLAY_DRAW];
    assert(s->seen==1000&&s->over12==30&&s->maximum==40000);
    assert(s->bins[7]==970&&s->bins[156]==20&&s->bins[255]==10);
    assert(rank_upper(s,50)==1023);
    assert(rank_upper(s,95)==1023);
    assert(rank_upper(s,99)==20095);
    ksn_p0_probe_sample(KSN_P0_OVERLAY_DRAW,12000);
    assert(s->over12==30); /* strictly above 12 ms */
    for(unsigned i=0;i<100;i++)ksn_p0_probe_sample(KSN_P0_OVERLAY_DRAW,50000);
    assert(rank_upper(s,99)==50000); /* tail quantile is bounded by max */
    ksn_p0_probe_transfer(64800,17);
    ksn_p0_probe_transfer(768,3);
    assert(transfer_frames==2&&transfer_bytes==65568&&transfer_bands==20);
    ksn_p0_probe_reset();
    assert(samples[KSN_P0_OVERLAY_DRAW].seen==0);
    assert(transfer_frames==0&&transfer_bytes==0&&transfer_bands==0);
    puts("p0 histogram: PASS (whole session, 128-us upper bounds, tail, deadline, LCD bytes/bands)");
    return 0;
}
