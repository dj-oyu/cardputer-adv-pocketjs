#include "pet_hub_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(i*8));}
static void packet(uint8_t *d,uint32_t seq,uint32_t tokens,uint32_t reset) {
    memset(d,0,48);d[0]=d[1]=1;d[3]=3;
    put32(d+4,seq);put32(d+8,123);put32(d+12,1700000000);
    put32(d+16,tokens);put32(d+24,reset);d[28]=0x10;d[29]=0x27;
    put32(d+36,32400);put32(d+40,1700000000);put32(d+44,pet_crc(d,44));
}
int main(int argc,char **argv) {
    pet_hub_t h;pet_hub_defaults(&h);uint8_t d[48];char label[25];
    assert(pet_crc((const uint8_t*)"123456789",9)==0xcbf43926u);
    packet(d,1,100000,1700000010);assert(pet_hub_packet(&h,d));assert(h.saved.food[0]==0);
    packet(d,2,101500,1700000010);assert(pet_hub_packet(&h,d));assert(h.saved.food[0]==1);
    assert(!pet_hub_packet(&h,d));assert(h.saved.food[0]==1);
    packet(d,3,1000,1700000010);assert(pet_hub_packet(&h,d));assert(h.saved.usage[0].tokens==101500);
    h.saved.selected=3;packet(d,4,102000,1700000010);assert(pet_hub_packet(&h,d));assert(h.saved.food[3]==1);
    d[0]^=1;pet_hub_saved_t before=h.saved;assert(!pet_hub_packet(&h,d));assert(!memcmp(&before,&h.saved,sizeof(before)));
    assert(!pet_hub_tick(&h,100,1700000009));assert(!pet_hub_take(&h,label));
    assert(pet_hub_tick(&h,200,1700000010));assert(pet_hub_take(&h,label));assert(!strcmp(label,"CODEX RESET TIME"));
    assert(!pet_hub_tick(&h,300,1700000011));assert(!pet_hub_take(&h,label));
    pet_hub_saved_t saved=h.saved;pet_hub_defaults(&h);h.saved=saved;
    assert(!pet_hub_tick(&h,0,1700000011));assert(!pet_hub_take(&h,label));
    packet(d,5,103000,1700000020);d[3]=1;put32(d+44,pet_crc(d,44));assert(pet_hub_packet(&h,d));
    assert(h.saved.usage[0].reset[0]==0&&h.saved.usage[0].used[0]==-1);
    assert(pet_hub_timer(&h,"test","BREAK TIME",1000));
    pet_hub_tick(&h,999,0);assert(!pet_hub_take(&h,label));
    pet_hub_tick(&h,1000,0);assert(pet_hub_take(&h,label));assert(!strcmp(label,"BREAK TIME"));
    pet_hub_tick(&h,1001,0);assert(!pet_hub_take(&h,label));
    assert(pet_hub_timer(&h,"cancel","NO",2000));assert(pet_hub_timer(&h,"cancel","NO",0));
    pet_hub_tick(&h,3000,0);assert(!pet_hub_take(&h,label));
    for(int i=0;i<8;i++)assert(pet_hub_notify(&h,"QUEUED"));
    assert(!pet_hub_notify(&h,"FULL"));
    assert(pet_hub_timer(&h,"full","RETRY",5000));pet_hub_tick(&h,5000,0);
    assert(pet_hub_take(&h,label));pet_hub_tick(&h,6000,0);
    for(int i=0;i<7;i++)assert(pet_hub_take(&h,label));
    assert(pet_hub_take(&h,label));assert(!strcmp(label,"RETRY"));
    pet_hub_defaults(&h);h.saved.utc_offset=0;h.saved.wake_minute=420;
    uint32_t morning=20000u*86400+7*3600;
    assert(pet_hub_tick(&h,0,morning));assert(pet_hub_take(&h,label));assert(!strcmp(label,"GOOD MORNING!"));
    assert(!pet_hub_tick(&h,0,morning+30));assert(!pet_hub_tick(&h,0,morning-3600));
    assert(!pet_hub_tick(&h,0,morning));assert(pet_hub_tick(&h,0,morning+86400));
    if(argc>1){FILE *f=fopen(argv[1],"rb");assert(f);assert(fread(d,1,48,f)==48);fclose(f);pet_hub_defaults(&h);assert(pet_hub_packet(&h,d));assert(h.saved.usage[1].tokens==12345);}
    puts("PASS: CRC/wire format, token baselines/replay, quota expiry, timers/cancel/queue pressure, daily alarm");
}
