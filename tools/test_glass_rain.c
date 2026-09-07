// Host: gcc -O2 -Wall -Wextra -Werror tools/test_glass_rain.c -lm -o .cache/test_glass_rain.exe
#include "../main/scene/glass_rain.c"
#include <assert.h>
#include <stdio.h>
static uint16_t original[RAIN_W*RAIN_H],whole[RAIN_W*RAIN_H],joined[RAIN_W*RAIN_H];
int main(void) {
    for(int y=0;y<RAIN_H;y++)for(int x=0;x<RAIN_W;x++)original[y*RAIN_W+x]=(uint16_t)((x*31/240)<<11 | (y*63/135)<<5 | (x+y)%32);
    unsigned wet=0,dry=0,shower_count=0;float prior=0;
    for(int frame=0;frame<9000;frame++) {
        glass_rain_prepare(1.0f/30,12345);
        if(shower_left>prior)shower_count++;
        prior=shower_left;
        if(frame%13)continue;
        unsigned active=0;
        for(int i=0;i<RAIN_N;i++)if(drops[i].life>0)active++;
        assert(active<=5);
        memcpy(whole,original,sizeof whole);glass_rain_draw(whole,0,RAIN_H);
        if(memcmp(whole,original,sizeof whole))wet++;else dry++;
        if(!active)assert(!memcmp(whole,original,sizeof whole));
        int strip=1+frame%11;
        for(int y=0;y<RAIN_H;y+=strip) {
            struct {uint16_t before[8],data[RAIN_W*11],after[8];} b;
            memset(&b,0xa5,sizeof b);int h=RAIN_H-y<strip?RAIN_H-y:strip;
            memcpy(b.data,original+y*RAIN_W,h*RAIN_W*2);
            glass_rain_draw(b.data,y,h);
            for(int i=0;i<8;i++)assert(b.before[i]==0xa5a5&&b.after[i]==0xa5a5);
            for(int i=h*RAIN_W;i<RAIN_W*11;i++)assert(b.data[i]==0xa5a5);
            memcpy(joined+y*RAIN_W,b.data,h*RAIN_W*2);
        }
        assert(!memcmp(whole,joined,sizeof whole));
    }
    assert(wet>60&&dry>wet&&shower_count>=6&&shower_count<=15);
    // Explicit edge cases: clipping, overlapping drops and the last LCD row.
    for(int edge=0;edge<2;edge++) {
        memset(drops,0,sizeof drops);
        drops[0]=(RainDrop){.x=edge?239:0,.y=134,.start=110,.age=2,.life=2,.radius=3};
        drops[1]=drops[0];
        memcpy(whole,original,sizeof whole);glass_rain_draw(whole,0,RAIN_H);
        memcpy(joined,original,sizeof joined);
        for(int y=0;y<RAIN_H;y++)glass_rain_draw(joined+y*RAIN_W,y,1);
        assert(!memcmp(whole,joined,sizeof whole));
    }
    memcpy(whole,original,sizeof whole);glass_rain_draw(whole,-1,8);
    assert(!memcmp(whole,original,sizeof whole));
    printf("RAIN_OK: 300 simulated seconds, %u showers, %u wet / %u dry samples; strips 1..11, bounds and overlap; state %zu bytes\n",
           shower_count,wet,dry,sizeof drops+sizeof source_row+sizeof rng+sizeof next_shower+sizeof shower_left+sizeof next_drop+sizeof remaining);
}
