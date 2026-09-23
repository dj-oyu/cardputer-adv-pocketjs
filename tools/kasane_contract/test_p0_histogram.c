#include <assert.h>
#include <stdio.h>
#define KASANE_P0_PROBE 1
#ifndef P0_TIMING_ONLY
#define KASANE_P0_COPY_PROBE 1
#endif
#include "../../main/ui/kasane/ksn_p0_probe.c"

int main(void){
    ksn_p0_probe_reset();
    for(unsigned i=0;i<970;i++)ksn_p0_probe_sample(KSN_P0_OVERLAY_DRAW,1000);
    for(unsigned i=0;i<20;i++)ksn_p0_probe_sample(KSN_P0_OVERLAY_DRAW,20000);
    for(unsigned i=0;i<10;i++)ksn_p0_probe_sample(KSN_P0_OVERLAY_DRAW,40000);
    const series *s=&samples[KSN_P0_OVERLAY_DRAW];
    const uint32_t draw_bucket=sample_bucket_us[KSN_P0_OVERLAY_DRAW];
    assert(s->seen==1000&&s->over12==30&&s->maximum==40000);
    assert(s->bins[7]==970&&s->bins[156]==20&&s->bins[255]==10);
    assert(rank_upper(s,50,draw_bucket)==1023);
    assert(rank_upper(s,95,draw_bucket)==1023);
    assert(rank_upper(s,99,draw_bucket)==20095);
    ksn_p0_probe_sample(KSN_P0_OVERLAY_DRAW,12000);
    assert(s->over12==30); /* strictly above 12 ms */
    for(unsigned i=0;i<100;i++)ksn_p0_probe_sample(KSN_P0_OVERLAY_DRAW,50000);
    assert(rank_upper(s,99,draw_bucket)==50000); /* tail quantile is bounded by max */
    ksn_p0_probe_sample(KSN_P0_UI_FRAME,40000);
    const series *frame=&samples[KSN_P0_UI_FRAME];
    assert(sample_bucket_us[KSN_P0_UI_FRAME]==1024);
    assert(frame->bins[39]==1);
    assert(rank_upper(frame,99,sample_bucket_us[KSN_P0_UI_FRAME])==40959);
    ksn_p0_probe_transfer(64800,17);
    ksn_p0_probe_transfer(768,3);
    assert(transfer_frames==2&&transfer_bytes==65568&&transfer_bands==20);
#ifdef KASANE_P0_BUS_PROBE
    ksn_p0_bus_begin_frame();
    ksn_p0_bus_phase_sample(KSN_P0_BUS_SWAP,100,false);
    uint32_t epoch=ksn_p0_bus_sd_epoch();
    ksn_p0_bus_sd_begin();
    assert(ksn_p0_bus_sd_active()&&ksn_p0_bus_sd_epoch()==epoch+1u);
    ksn_p0_bus_phase_sample(KSN_P0_BUS_REAP,300,true);
    ksn_p0_bus_sd_end();
    assert(!ksn_p0_bus_sd_active());
    ksn_p0_bus_phase_sample(KSN_P0_BUS_QUEUE,200,false);
    ksn_p0_bus_end_frame(1000);
    assert(samples[KSN_P0_LCD_SWAP].seen==1&&samples[KSN_P0_LCD_SWAP].maximum==100);
    assert(samples[KSN_P0_LCD_REAP].seen==1&&samples[KSN_P0_LCD_REAP].maximum==300);
    assert(samples[KSN_P0_LCD_QUEUE].seen==1&&samples[KSN_P0_LCD_QUEUE].maximum==200);
    assert(samples[KSN_P0_LCD_OTHER].seen==1&&samples[KSN_P0_LCD_OTHER].maximum==400);
    assert(samples[KSN_P0_LCD_SEND_SD].seen==1&&samples[KSN_P0_LCD_SEND_IDLE].seen==0);
    assert(bus_sd_reap_calls==1);
#endif
#ifndef P0_TIMING_ONLY
    ksn_p0_probe_copy(KSN_P0_ADAPTER_SLOT_COMMIT,12);
    ksn_p0_probe_copy(KSN_P0_CORE_SUBMIT_COMMAND,24);
    ksn_p0_probe_copy(KSN_P0_CORE_SUBMIT_COMMAND,24);
    ksn_p0_probe_copy(KSN_P0_CORE_SUBMIT_COMMAND,0);
    assert(copy_calls[KSN_P0_ADAPTER_SLOT_COMMIT]==1&&
           copy_bytes[KSN_P0_ADAPTER_SLOT_COMMIT]==12);
    assert(copy_calls[KSN_P0_CORE_SUBMIT_COMMAND]==2&&
           copy_bytes[KSN_P0_CORE_SUBMIT_COMMAND]==48);
    assert(copy_groups[KSN_P0_ADAPTER_SLOT_COMMIT]==P0_GROUP_ADAPTER_SCHEMA);
    assert(copy_groups[KSN_P0_CORE_SUBMIT_COMMAND]==P0_GROUP_CORE_SUBMIT);
    uint64_t grouped_bytes[P0_GROUP_COUNT]={0};
    uint32_t grouped_calls[P0_GROUP_COUNT]={0};
    for(unsigned i=0;i<KSN_P0_COPY_COUNT;i++){
        assert(copy_groups[i]<P0_GROUP_COUNT);
        grouped_bytes[copy_groups[i]]+=copy_bytes[i];
        grouped_calls[copy_groups[i]]+=copy_calls[i];
    }
    assert(grouped_bytes[P0_GROUP_ADAPTER_SCHEMA]==12&&
           grouped_calls[P0_GROUP_ADAPTER_SCHEMA]==1);
    assert(grouped_bytes[P0_GROUP_CORE_SUBMIT]==48&&
           grouped_calls[P0_GROUP_CORE_SUBMIT]==2);
#else
    ksn_p0_probe_copy(KSN_P0_CORE_SUBMIT_COMMAND,24);
#endif
    ksn_p0_probe_reset();
    assert(samples[KSN_P0_OVERLAY_DRAW].seen==0);
#ifdef KASANE_P0_BUS_PROBE
    assert(samples[KSN_P0_LCD_REAP].seen==0&&bus_sd_reap_calls==0);
#endif
    assert(transfer_frames==0&&transfer_bytes==0&&transfer_bands==0);
#ifndef P0_TIMING_ONLY
    assert(copy_calls[KSN_P0_CORE_SUBMIT_COMMAND]==0&&
           copy_bytes[KSN_P0_CORE_SUBMIT_COMMAND]==0);
#endif
    puts("p0 histogram: PASS (whole session, 128-us upper bounds, tail, deadline, LCD bytes/bands)");
    return 0;
}
